#include "ES8311AudioSink.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ES8311";

// ---------------------------------------------------------------------------
// ES8311 register map (addresses match es8311.h from bell)
// ---------------------------------------------------------------------------
#define REG_RESET       0x00
#define REG_CLK01       0x01
#define REG_CLK02       0x02
#define REG_CLK03       0x03
#define REG_CLK04       0x04
#define REG_CLK05       0x05
#define REG_CLK06       0x06
#define REG_CLK07       0x07
#define REG_CLK08       0x08
#define REG_SDPIN       0x09
#define REG_SDPOUT      0x0A
#define REG_SYS0D       0x0D
#define REG_SYS0E       0x0E
#define REG_SYS12       0x12
#define REG_SYS13       0x13
#define REG_ADC1C       0x1C
#define REG_CLK06       0x06
#define REG_CLK07       0x07
#define REG_CLK08       0x08
#define REG_DAC31       0x31
#define REG_DAC32       0x32
#define REG_DAC37       0x37

// ---------------------------------------------------------------------------

void ES8311AudioSink::i2cWrite(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(codec, buf, 2, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c write reg 0x%02x failed: %s", reg, esp_err_to_name(err));
    }
}

void ES8311AudioSink::codecInit() {
    // Full reset sequence (from Espressif ES8311 reference driver)
    i2cWrite(REG_RESET, 0x1F);  // reset all registers
    vTaskDelay(pdMS_TO_TICKS(20));
    i2cWrite(REG_RESET, 0x00);  // release reset
    i2cWrite(REG_RESET, 0x80);  // power-up command

    // Clock: MCLK from MCLK pin, enable all clocks
    // MCLK = 256 * 44100 = 11289600 Hz
    i2cWrite(REG_CLK01, 0x3F);  // all clock gates enabled, MCLK from MCLK pin

    // Clock dividers for 11289600 Hz / 44100 Hz
    // From coeff table: pre_div=1, pre_multi=0, adc_div=1, dac_div=1
    //   fs_mode=0, lrck_h=0x00, lrck_l=0xFF, bclk_div=4, osr=0x10
    i2cWrite(REG_CLK02, 0x00);  // pre_div=1, pre_multi=0
    i2cWrite(REG_CLK03, 0x10);  // adc_osr = 0x10
    i2cWrite(REG_CLK04, 0x10);  // dac_osr = 0x10
    i2cWrite(REG_CLK05, 0x00);  // adc_div=1, dac_div=1
    i2cWrite(REG_CLK06, 0x03);  // bclk_div = 4 → store (4-1) = 3
    i2cWrite(REG_CLK07, 0x00);  // lrck_h = 0x00
    i2cWrite(REG_CLK08, 0xFF);  // lrck_l = 0xFF

    // Serial data format: slave mode, I2S, 16-bit
    // reg09 bits[5:3] = 011 (16-bit), bits[1:0] = 00 (I2S/Philips) → 0x0C
    i2cWrite(REG_SDPIN,  0x0C); // DAC input  : 16-bit I2S
    i2cWrite(REG_SDPOUT, 0x0C); // ADC output : 16-bit I2S

    // System / power
    i2cWrite(REG_SYS0D, 0x01);  // power up analog circuitry
    i2cWrite(REG_SYS0E, 0x02);  // enable analog PGA + ADC modulator
    i2cWrite(REG_SYS12, 0x00);  // power up DAC
    i2cWrite(REG_SYS13, 0x10);  // enable output to HP driver

    // ADC / DAC signal path
    i2cWrite(REG_ADC1C, 0x6A);  // ADC equalizer bypass, cancel DC offset
    i2cWrite(REG_DAC37, 0x08);  // bypass DAC equalizer

    // Volume (initialise to max; volumeChanged() will set the real value)
    i2cWrite(REG_DAC32, 0xBF);
}

void ES8311AudioSink::i2sInit(uint32_t sampleRate) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sampleRate,
            .clk_src        = I2S_CLK_SRC_DEFAULT,  // ESP32-S3 has no APLL; use PLL
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_PIN,
            .bclk = (gpio_num_t)I2S_BCK_PIN,
            .ws   = (gpio_num_t)I2S_WS_PIN,
            .dout = (gpio_num_t)I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
}

ES8311AudioSink::ES8311AudioSink() {
    softwareVolumeControl = false;  // we handle volume in hardware

    // PA: pull low initially to avoid pop
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << PA_CTRL_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level((gpio_num_t)PA_CTRL_PIN, 0);

    // I2C bus
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = I2C_NUM_0,
        .sda_io_num    = (gpio_num_t)I2C_SDA_PIN,
        .scl_io_num    = (gpio_num_t)I2C_SCL_PIN,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    // ES8311 device on the bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8311_ADDR,
        .scl_speed_hz    = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &codec));

    // Initialise codec registers then I2S
    codecInit();
    i2sInit(44100);

    // Enable amplifier after init to suppress startup pop
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)PA_CTRL_PIN, 1);

    ESP_LOGI(TAG, "ES8311 ready");
}

ES8311AudioSink::~ES8311AudioSink() {
    gpio_set_level((gpio_num_t)PA_CTRL_PIN, 0);
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
    }
    if (codec)   i2c_master_bus_rm_device(codec);
    if (i2c_bus) i2c_del_master_bus(i2c_bus);
}

void ES8311AudioSink::feedPCMFrames(const uint8_t* buffer, size_t bytes) {
    if (!tx_chan || bytes == 0) return;
    size_t written = 0;
    i2s_channel_write(tx_chan, buffer, bytes, &written, portMAX_DELAY);
}

void ES8311AudioSink::volumeChanged(uint16_t volume) {
    // volume is 0-255 from cspot; map to ES8311 DAC32 range 0-0xFF
    uint8_t regval = (volume > 255) ? 255 : (uint8_t)volume;
    if (regval == 0) regval = 1;  // 0 = mute in ES8311
    i2cWrite(REG_DAC32, regval);
}

bool ES8311AudioSink::setParams(uint32_t sampleRate, uint8_t channelCount, uint8_t bitDepth) {
    // cspot always uses 44100/2/16 so just return true
    (void)channelCount; (void)bitDepth;
    if (sampleRate != 44100) {
        ESP_LOGW(TAG, "setParams: only 44100 Hz supported, got %lu", (unsigned long)sampleRate);
    }
    return true;
}
