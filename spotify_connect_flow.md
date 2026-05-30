# Spotify Connect Flow — cspot on ESP32-S3

## Overview

Complete flow from device discovery to audio output, as implemented in cspot running on ESP32-S3 with IDF 6.0.

---

## 1. Zeroconf Discovery (mDNS + HTTP)

- Device advertises `_spotify-connect._tcp` via mDNS (port 8080)
- Spotify app GETs `/spotify_info` → receives device's DH public key + device metadata (JSON)
- Spotify app POSTs `/spotify_info` with:
  - `blob` — AES-encrypted authentication blob (base64)
  - `clientKey` — Spotify app's DH public key (base64, 96 bytes)
  - `userName` — Spotify username

The HTTP server (civetweb) runs for the entire device lifetime, so re-auth POSTs can arrive
at any time (e.g. the Spotify app reconnects after the device is selected again).

---

## 2. Blob Decoding (LoginBlob)

Goal: extract `username`, `authType`, and `authData` (the stored credential).

```
sharedKey  = clientKey ^ devicePrivKey mod DHPrime       (DH, 96 bytes)
baseKey    = SHA1(sharedKey)[0:16]                        (16 bytes)
checksumKey = HMAC-SHA1(baseKey, "checksum")
encryptionKey = HMAC-SHA1(baseKey, "encryption")[0:16]

Verify: HMAC-SHA1(checksumKey, blob[16:-20]) == blob[-20:]
Decrypt: AES-CTR(encryptionKey, iv=blob[0:16], blob[16:-20])

Second layer (decodeBlobSecondary):
  secret   = SHA1(deviceId)
  pbkKey   = PBKDF2-HMAC-SHA1(secret, username, 256 iterations, 20 bytes)
  key      = SHA1(pbkKey) + 0x00000014
  Decrypt AES-ECB(key, partDecoded)
  XOR-unfold the result

Result fields: username, authType=1 (STORED_SPOTIFY_CREDENTIALS), authData (152 bytes)
```

**mbedtls 4.0 / IDF 6.0 gotchas fixed here:**
- `mbedtls_md_setup(ctx, SHA1, hmac=0)` required for plain hash (hmac=1 silently returns zeros)
- `sha1HMAC` must be manual RFC 2104 (keys >64 bytes hashed first; mbedtls 4.0 PSA hmac path broken)
- `pbkdf2HmacSha1` must be manual (mbedtls built-in uses broken hmac=1 path internally)

---

## 3. AP Connection — Shannon Cipher (PlainConnection + Session)

- Resolve AP addresses via HTTPS `apresolve.spotify.com`
- TCP connect to AP e.g. `ap-gew4.spotify.com:4070` — **plain TCP, no TLS**
- ClientHello / APHello DH handshake → derive Shannon stream cipher keys
- All subsequent AP traffic is Shannon-encrypted packets
- Send `ClientResponseEncrypted` with `username` + `authData`
- AP replies `APWelcome` → **Authorization successful**

---

## 4. Mercury Subscriptions (MercurySession)

Mercury is Spotify's internal pub/sub + request/response protocol over the Shannon connection.

- Subscribe to `hm://remote/3/user/{username}/` → receives Spirc control frames
- Subscribe to `hm://social-connect/v2/events/...` → Connect session events
- Mercury also handles: metadata requests, audio key requests
- **Mercury echoes your own SEND messages back to you** — the Notify handler must filter
  out self-notifications (compare `remoteFrame.device_state.name` with own device name)

---

## 5. Spirc — Remote Control (SpircHandler)

Spirc ("Spirit Controller") is Spotify's Connect remote control protocol.

- Send `kMessageTypeHello` to announce device is ready for playback
- Receive `kMessageTypeLoad` when user selects a track → track URI passed to TrackQueue
- Receive `kMessageTypePlay` / `kMessageTypePause` / `kMessageTypeSeek`
- Receive `kMessageTypeVolume` → volume in 0–65535; map to hardware range (ES8311: >> 8 → 0–255)
- Receive `kMessageTypeNotify` from another device → check name ≠ ours before firing DISC

---

## 6. Track Resolution (TrackQueue)

For each track to play:

1. **Metadata** — Mercury GET `hm://metadata/3/track/{trackId}` → protobuf  
   - Track name, duration, restrictions (by country), list of `(fileId, format)` pairs
2. **File selection** — pick `fileId` matching configured audio format  
   - Formats: OGG_VORBIS_96/160/320, MP3_256/320, AAC_24/48
3. **Audio key** — `get_audio_key` Shannon packet → 16-byte AES key for this track
4. **Bearer token** — HTTPS POST `https://accounts.spotify.com/api/token`  
   - `grant_type=client_credentials` with your registered Spotify app's client_id + client_secret  
   - The old `login5.spotify.com` path with web-player client_id now returns 429 (rate-limited)  
   - Returns OAuth2 Bearer token (expires ~1 hour)
5. **CDN URL** — HTTPS GET `https://api.spotify.com/v1/storage-resolve/files/audio/interactive/{fileId}?alt=json&product=9`  
   - Authorization: `Bearer {token}`  
   - Returns JSON `{"cdnurl": ["https://...cdn..."]}`

**TLS gotchas fixed here (IDF 6.0 / mbedtls 4.0):**
- `psa_crypto_init()` must be called before any TLS handshake
- `mbedtls_ssl_conf_max_tls_version(..., MBEDTLS_SSL_VERSION_TLS1_2)` required — TLS 1.3 PSA key-share leaks `PSA_ERROR_INVALID_ARGUMENT` (-135) on ESP32-S3
- `MBEDTLS_DEFAULT_MEM_ALLOC` required in sdkconfig — default `MBEDTLS_INTERNAL_MEM_ALLOC` exhausts internal RAM, causing `PSA_ERROR_INSUFFICIENT_MEMORY` (-141) in `mbedtls_ssl_setup`

---

## 7. Audio Streaming (CDNAudioFile)

- HTTPS range-request to CDN URL
- File is split into 4096-byte chunks
- Each chunk decrypted: AES-128 CTR, IV derived from chunk offset
- Decrypted bytes fed to codec decoder

---

## 8. Audio Output (ES8311AudioSink → I2S)

- Decoded PCM written through CircularBuffer
- I2S driver sends PCM to ES8311 codec (slave mode, 44100 Hz, 16-bit stereo, Philips format)
- ES8311 converts to analog → NS4150 class-D amplifier → speaker
- Pin mapping (Waveshare ESP32-S3-LCD-1.54):
  - I2S: MCLK=8, BCK=9, WS=10, DOUT=**12** (IO12=DSDIN, DAC input TO codec — IO11=ASDOUT is ADC output FROM codec)
  - I2C: SCL=41, SDA=42
  - PA_CTRL=7 (NS4150 amplifier enable, active high)

**ES8311 init gotchas:**
- `REG_SYS0D = 0x01` must be written to power up analog circuitry (missing = complete silence)
- Full reset sequence: `0x1F` → 20ms delay → `0x00` → `0x80`
- `REG_CLK01 = 0x3F` (enable all clock gates) — `0x30` leaves some disabled
- Registers 0x06/0x07/0x08 (BCLK/LRCK dividers) must be set for the target sample rate
- `REG_SDPIN = 0x0C` — DAC input format: 16-bit I2S (not written = wrong default)

---

## 9. ESP32-S3 PSRAM Stability (IDF 6.0)

**Root cause:** ESP32-S3 has no hardware cache coherency between Core 0 and Core 1 for PSRAM
(external OPI SRAM). Any task running on Core 1 with a PSRAM stack will experience corrupted
return addresses when Core 0 performs DMA/cache operations (WiFi, I2S).

**Fixes applied:**

| Problem | Root cause | Fix |
|---------|-----------|-----|
| cspotMainTask PSRAM stack | `bell::Task` heap-alloc fell back to PSRAM when fragmented | 32 KB static `DRAM_ATTR` stack via `xTaskCreateStaticPinnedToCore` |
| civetweb worker stacks in PSRAM | Hardcoded `MALLOC_CAP_SPIRAM` in civetweb's ESP_PLATFORM path | Changed to `MALLOC_CAP_INTERNAL`; reduced to 8 KB; limited to 1 worker thread |
| civetweb tasks on Core 1 | Hardcoded core=1 in civetweb's ESP_PLATFORM path | Changed to core=0 |
| civetweb `InstrFetchProhibited` | Dangling pointer: `&local_var` passed as FreeRTOS task parameter after function return | `mg_start_thread_with_id` writes directly to `*threadidptr` (ctx storage); `mg_start_thread` heap-allocates handle |
| TrackPlayer 48 KB stack OOM | After civetweb fixes, 32 KB civetweb × 2 + 32 KB TrackQueue fragmented heap | Reduced TrackPlayer stack from 48 KB to 32 KB |
| nanopb Python version conflict | `protobuf 4.x` removed `MakeClass`; downgrade caused different error | Bypass generator: pre-generated `.pb.c`/`.pb.h` in `proto_generated/protobuf/` |
| `timegm` duplicate symbol | picolibc (IDF 6.0) + civetweb both define `timegm` | `HAVE_TIMEGM` defined for bell; civetweb.c wrapped in `#ifndef HAVE_TIMEGM` |
| GCC 15.2.0 ICE in esp_lcd | `-fzero-init-padding-bits=all` triggers RTL/ira segfault | `target_compile_options(__idf_esp_lcd PRIVATE -fzero-init-padding-bits=standard)` |

**sdkconfig.defaults keys:**
```
CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=n   # fail-fast on any PSRAM stack attempt
CONFIG_PTHREAD_DEFAULT_CORE_0=y               # pthreads stay on Core 0 (defensive)
CONFIG_BT_ENABLED=n                           # free ~150 KB internal RAM
CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y            # TLS buffers go to PSRAM, not internal
```

---

## Implementation Status

| Step | Status |
|------|--------|
| Zeroconf / mDNS | ✅ Working |
| Blob decode (LoginBlob) | ✅ Working |
| AP connection + Shannon auth | ✅ Working |
| Mercury subscriptions | ✅ Working |
| Spirc hello + load | ✅ Working |
| Track metadata | ✅ Working |
| Audio key | ✅ Working |
| Bearer token (client_credentials) | ✅ Working |
| CDN URL (storage-resolve) | ✅ Working |
| CDN audio download + decrypt | ✅ Working |
| OGG Vorbis decode → PCM | ✅ Working |
| ES8311 audio output | ✅ Working |
| Volume control (slider) | ✅ Working (0–65535 >> 8 → ES8311) |
| Pause / resume | ✅ Working |
| Track skip | ✅ Working |
| Stop when another player takes over | ✅ Working (DISC event, self-Notify filtered) |
| Switch back to device mid-session | ⚠️ Partial — works if TCP session alive; if Spotify re-does Zeroconf auth the outer loop does not restart the session |
| Stable restart (no crash on reboot) | ✅ Fixed |

---

## Key Files Modified

| File | Change |
|------|--------|
| `bell/main/utilities/Crypto.cpp` | Manual SHA1/HMAC (hmac=0 for plain hash), manual PBKDF2 |
| `bell/main/utilities/include/BellTask.h` | Fail-fast abort instead of PSRAM stack fallback; `esp_ptr_internal` check |
| `bell/main/io/TLSSocket.cpp` | `psa_crypto_init()`, force TLS 1.2, re-init ssl/conf on each `open()` |
| `bell/main/io/HTTPClient.cpp` | Expose `statusCode()` on Response |
| `bell/external/civetweb/civetweb.c` | PSRAM→INTERNAL stack; Core 1→Core 0; dangling-pointer fix; 16 KB→8 KB stack; `HAVE_TIMEGM` guard |
| `bell/main/io/BellHTTPServer.cpp` | `num_threads=1` option to limit heap fragmentation |
| `cspot/include/CSpotContext.h` | Add `clientId` / `clientSecret` to ConfigState |
| `cspot/src/AccessKeyFetcher.cpp` | Switch to OAuth2 client_credentials flow |
| `cspot/src/LoginBlob.cpp` | Guards against empty/corrupt blob decode |
| `cspot/src/SpircHandler.cpp` | DISC on takeover (with self-Notify filter); volume event |
| `cspot/src/TrackPlayer.cpp` | Stack 48 KB → 32 KB |
| `cspot/src/TrackQueue.cpp` | CDN URL retry on 429, proper error handling |
| `cspot/CMakeLists.txt` | Pre-generated proto fallback (`proto_generated/protobuf/`) |
| `main/main.cpp` | Static DRAM stack; esp_pthread INTERNAL config; volume + DISC handlers |
| `main/ES8311AudioSink.cpp` | Correct init sequence (REG_SYS0D analog power-up, full clock config) |
| `main/ES8311AudioSink.h` | DOUT=12 (DSDIN), not 11 (ASDOUT) |
| `main/CMakeLists.txt` | `HAVE_TIMEGM` for bell; GCC ICE workaround for esp_lcd |
| `sdkconfig.defaults` | BT=n, FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=n, PTHREAD_DEFAULT_CORE_0, MBEDTLS_DEFAULT_MEM_ALLOC, asymmetric SSL buffers |
| `proto_generated/protobuf/` | Pre-generated .pb.c/.pb.h bypassing nanopb Python generator |
