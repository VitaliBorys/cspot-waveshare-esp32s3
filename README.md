# Spotify Connect — Waveshare ESP32-S3-LCD-1.54

Spotify Connect player running on the [Waveshare ESP32-S3-LCD-1.54](https://www.waveshare.com/esp32-s3-lcd-1.54.htm) development board. Select the device in the Spotify app and audio plays through the onboard ES8311 codec and NS4150 amplifier.

Built with [cspot](https://github.com/feelfreelinux/cspot) and [ESP-IDF 6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/).

---

## Hardware

| Component | Details |
|-----------|---------|
| Board | Waveshare ESP32-S3-LCD-1.54 |
| SoC | ESP32-S3R8 — dual-core 240 MHz, 8 MB OPI PSRAM, 16 MB flash |
| Audio codec | ES8311 (I2C config + I2S audio) |
| Amplifier | NS4150 class-D → onboard speaker |

Pin mapping:

| Signal | GPIO |
|--------|------|
| I2S MCLK | 8 |
| I2S BCK | 9 |
| I2S WS | 10 |
| I2S DOUT (→ codec) | **12** (DSDIN) |
| I2C SCL | 41 |
| I2C SDA | 42 |
| PA enable | 7 |

---

## Features

- Spotify Connect — appears as a device in the Spotify app
- OGG Vorbis / MP3 / AAC playback
- Volume control via Spotify slider
- Pause / resume / track skip
- Stops when another Spotify device takes over
- Stable on reboot — no crash on restart

---

## Requirements

- [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/get-started/)
- Spotify Premium account
- Registered Spotify app at [developer.spotify.com](https://developer.spotify.com/dashboard) (free) — needed for the client credentials OAuth2 flow used to fetch CDN tokens

---

## Setup

### 1. Clone with submodules

```bash
git clone --recurse-submodules https://github.com/VitaliBorys/cspot-waveshare-esp32s3
cd cspot-waveshare-esp32s3
```

### 2. Create credentials file

```bash
cp main/credentials.h.example main/credentials.h
```

Edit `main/credentials.h`:

```cpp
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"
#define SPOTIFY_CLIENT_ID     "your_spotify_client_id"
#define SPOTIFY_CLIENT_SECRET "your_spotify_client_secret"
```

`credentials.h` is gitignored — never committed.

### 3. Build and flash

```bash
idf.py build flash monitor
```

On first boot the device connects to WiFi, starts an HTTP server on port 8080, and registers as `SpotifyESP32` via mDNS. Open Spotify, go to **Devices**, and select **SpotifyESP32**.

---

## Clean rebuild

If you need a full rebuild from scratch:

```bash
rm -rf build sdkconfig managed_components
idf.py build flash monitor
```

---

## Project structure

```
main/
  main.cpp              — app entry point, Spotify session, audio loop
  ES8311AudioSink.cpp/h — I2S + ES8311 codec driver
  credentials.h.example — template for WiFi + Spotify credentials
  CMakeLists.txt
lib/cspot/              — cspot submodule (VitaliBorys/cspot fork)
  cspot/bell/           — bell submodule (VitaliBorys/bell fork)
proto_generated/        — pre-generated protobuf C files
sdkconfig.defaults      — ESP-IDF build configuration
```

---

## Documentation

- [`spotify_connect_flow.md`](spotify_connect_flow.md) — end-to-end flow from device discovery to audio output, including all IDF 6.0 compatibility fixes
- [`cspot_protocol.md`](cspot_protocol.md) — threading model, Mercury/Spirc protocol reference, event types, data path

---

## Submodule forks

This project uses modified forks of cspot and bell with fixes required for ESP-IDF 6.0 / mbedtls 4.0 and ESP32-S3 hardware:

| Repo | Fork | Changes |
|------|------|---------|
| [feelfreelinux/cspot](https://github.com/feelfreelinux/cspot) | [VitaliBorys/cspot](https://github.com/VitaliBorys/cspot) | OAuth2 CDN auth, stack sizes, DISC event, reconnect fix |
| [feelfreelinux/bell](https://github.com/feelfreelinux/bell) | [VitaliBorys/bell](https://github.com/VitaliBorys/bell) | TLS 1.2, mbedtls 4.0 crypto, civetweb stability fixes |

---

## License

cspot and bell are MIT licensed. See their respective repositories.  
This project's own code (`main/`) follows the same MIT license.
