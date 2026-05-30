#include <atomic>
#include <memory>
#include <string>

#include "BellHTTPServer.h"
#include "BellTask.h"
#include "BellUtils.h"
#include "CircularBuffer.h"
#include "CSpotContext.h"
#include "ES8311AudioSink.h"
#include "Logger.h"
#include "LoginBlob.h"
#include "MDNSService.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pthread.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

// ---------------------------------------------------------------------------
// Credentials — copy main/credentials.h.example to main/credentials.h
// and fill in your WiFi + Spotify app credentials (file is gitignored)
// ---------------------------------------------------------------------------
#include "credentials.h"
#define DEVICE_NAME "SpotifyESP32"

static const char* TAG = "main";

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void wifiConnect() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wcfg = {};
    strncpy((char*)wcfg.sta.ssid,     WIFI_SSID, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char*)wcfg.sta.password, WIFI_PASS,  sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();

    // Wait for IP (max 15 s)
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (int i = 0; i < 30; i++) {
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ip.ip));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Wait for DNS server (DHCP sets it with the IP, but lwIP may lag slightly)
    esp_netif_dns_info_t dns = {};
    for (int i = 0; i < 20; i++) {
        if (netif && esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK
                && dns.ip.u_addr.ip4.addr != 0) {
            ESP_LOGI(TAG, "DNS ready: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "WiFi/DNS timeout — continuing anyway");
}

// ---------------------------------------------------------------------------
// SPIFFS (stores Spotify auth blob)
// ---------------------------------------------------------------------------
static void spiffsInit() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    }
}

// ---------------------------------------------------------------------------
// cspot player task — owns the audio sink and circular buffer
// ---------------------------------------------------------------------------
class CSpotPlayer : public bell::Task {
    std::shared_ptr<cspot::SpircHandler> handler;
    std::unique_ptr<ES8311AudioSink>     audioSink;
    std::unique_ptr<bell::CircularBuffer> circBuf;
    std::atomic<bool> paused{false};

public:
    CSpotPlayer(std::shared_ptr<cspot::SpircHandler> h)
        : bell::Task("cspot_player", 16 * 1024, 0, 0), handler(h) {

        audioSink = std::make_unique<ES8311AudioSink>();
        audioSink->setParams(44100, 2, 16);
        audioSink->volumeChanged(160);

        circBuf = std::make_unique<bell::CircularBuffer>(1024 * 128);

        handler->getTrackPlayer()->setDataCallback(
            [this](uint8_t* data, size_t bytes, std::string_view) -> size_t {
                feed(data, bytes);
                return bytes;
            });

        handler->setEventHandler(
            [this](std::unique_ptr<cspot::SpircHandler::Event> ev) {
                switch (ev->eventType) {
                    case cspot::SpircHandler::EventType::PLAY_PAUSE:
                        paused = std::get<bool>(ev->data);
                        break;
                    case cspot::SpircHandler::EventType::VOLUME:
                        // Spotify sends 0-65535; ES8311 volumeChanged expects 0-255
                        audioSink->volumeChanged((uint16_t)(std::get<int>(ev->data) >> 8));
                        break;
                    case cspot::SpircHandler::EventType::DISC:
                        paused = true;
                        circBuf->emptyBuffer();
                        break;
                    case cspot::SpircHandler::EventType::FLUSH:
                    case cspot::SpircHandler::EventType::SEEK:
                    case cspot::SpircHandler::EventType::PLAYBACK_START:
                        circBuf->emptyBuffer();
                        break;
                    default: break;
                }
            });

        startTask();
    }

    void feed(uint8_t* data, size_t len) {
        size_t rem = len;
        while (rem > 0) {
            size_t w = circBuf->write(data + (len - rem), rem);
            if (w == 0) BELL_SLEEP_MS(10);
            rem -= w;
        }
    }

    void runTask() override {
        std::vector<uint8_t> buf(1024);
        while (true) {
            if (!paused) {
                size_t n = circBuf->read(buf.data(), buf.size());
                if (n > 0)
                    audioSink->feedPCMFrames(buf.data(), n);
                else
                    BELL_SLEEP_MS(10);
            } else {
                BELL_SLEEP_MS(100);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// cspot main task — Zeroconf auth then start player
//
// Stack is a static global so the linker places it in internal DRAM (BSS).
// This avoids BellTask's heap-based PSRAM fallback, which causes an
// ESP32-S3 inter-core cache-coherency crash when the stack lands in PSRAM.
// ---------------------------------------------------------------------------
// DRAM_ATTR (.dram0.data) forces placement in internal DRAM.
// .dram0.bss is an orphan section in IDF 6.0's linker script and cannot be used.
static DRAM_ATTR StaticTask_t s_cspot_tcb;
static DRAM_ATTR StackType_t  s_cspot_stack[32 * 1024];

static void cspotMainTask(void*) {
    // Force all pthreads spawned from this task (civetweb master + workers) to allocate
    // their stacks in internal RAM.  PSRAM stacks cause ESP32-S3 inter-core cache
    // coherency crashes.  With inherit_cfg=true the setting propagates to child threads.
    {
        esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
        pcfg.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        pcfg.inherit_cfg      = true;
        esp_pthread_set_cfg(&pcfg);
    }

    mdns_init();
    mdns_hostname_set("cspot");

    std::atomic<bool> gotBlob{false};
    auto blob   = std::make_shared<cspot::LoginBlob>(DEVICE_NAME);
    auto server = std::make_unique<bell::BellHTTPServer>(8080);

    server->registerGet("/spotify_info",
        [&server, blob](struct mg_connection* conn) {
            return server->makeJsonResponse(blob->buildZeroconfInfo());
        });

    server->registerPost("/spotify_info",
        [&server, blob, &gotBlob](struct mg_connection* conn) {
            nlohmann::json obj;
            obj["status"]       = 101;
            obj["spotifyError"] = 0;
            obj["statusString"] = "ERROR-OK";

            auto info = mg_get_request_info(conn);
            if (info->content_length > 0) {
                std::string body(info->content_length, '\0');
                mg_read(conn, body.data(), info->content_length);

                mg_header hd[16];
                int n = mg_split_form_urlencoded(body.data(), hd, 16);

                std::map<std::string, std::string> qmap;
                for (int i = 0; i < n; i++) {
                    if (!hd[i].name || !hd[i].value) continue;
                    std::string raw(hd[i].value);
                    std::string decoded(raw.size() + 1, '\0');
                    int len = mg_url_decode(raw.c_str(), (int)raw.size(),
                                            decoded.data(), (int)decoded.size(), 0);
                    std::string val = (len >= 0) ? decoded.substr(0, len) : raw;
                    qmap[hd[i].name] = val;
                }

                blob->loadZeroconfQuery(qmap);
                if (!blob->getUserName().empty())
                    gotBlob = true;
                else
                    ESP_LOGE(TAG, "Blob auth failed — credentials not loaded");
            }
            return server->makeJsonResponse(obj.dump());
        });

    bell::MDNSService::registerService(
        blob->getDeviceName(), "_spotify-connect", "_tcp", "", 8080,
        {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});

    ESP_LOGI(TAG, "Waiting for Spotify app…");
    while (!gotBlob) BELL_SLEEP_MS(1000);
    ESP_LOGI(TAG, "Got credentials, starting player");

    auto ctx = cspot::Context::createFromBlob(blob);
    ctx->config.clientId     = SPOTIFY_CLIENT_ID;
    ctx->config.clientSecret = SPOTIFY_CLIENT_SECRET;

    // DNS / TLS may fail transiently at startup — retry a few times
    for (int retry = 0; ; retry++) {
        try {
            ctx->session->connectWithRandomAp();
            break;
        } catch (std::exception& e) {
            if (retry >= 9) throw;
            ESP_LOGW(TAG, "AP connect failed (%s), retry %d/10", e.what(), retry + 1);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    auto token = ctx->session->authenticate(blob);

    if (!token.empty()) {
        ctx->session->startTask();
        auto handler = std::make_shared<cspot::SpircHandler>(ctx);
        handler->subscribeToMercury();
        auto player = std::make_shared<CSpotPlayer>(handler);
        while (true) ctx->session->handlePacket();
        handler->disconnect();
    }

    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
extern "C" void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    spiffsInit();
    wifiConnect();

    bell::setDefaultLogger();
    xTaskCreateStaticPinnedToCore(
        cspotMainTask, "cspot_main", sizeof(s_cspot_stack), nullptr,
        5, s_cspot_stack, &s_cspot_tcb, 1);
    vTaskSuspend(NULL);
}
