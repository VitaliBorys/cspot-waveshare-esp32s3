# cspot Internal Protocol Reference

How cspot implements Spotify Connect on ESP32-S3. Covers the threading model,
event flow, message types, and the data path from Spotify servers to the speaker.

---

## Threading Model

```
Core 0                              Core 1
──────────────────────────          ──────────────────────────
WiFi (prio 23)                      cspotMainTask (prio 5)
lwIP / netif                          │  32 KB static DRAM stack
civetweb-master  (prio 5)             │  runs: Zeroconf auth, session connect,
civetweb-worker  (prio 5)             │  Mercury packet loop
                                      │
                                      ├─ MercurySession bell::Task (prio 8, 4 KB, Core 1)
                                      │    reads Shannon packets, dispatches Mercury
                                      │
                                      ├─ CSpotTrackQueue bell::Task (prio 7, 32 KB, Core 1)
                                      │    resolves metadata → audio key → CDN URL
                                      │
                                      ├─ cspot_player bell::Task (prio 10, 32 KB, Core 1)
                                      │    HTTP CDN streaming + codec (Vorbis/AAC/MP3)
                                      │
                                      └─ cspot_player (app) bell::Task (prio 5, 16 KB, Core 0)
                                           reads CircularBuffer → feedPCMFrames → I2S
```

**Key constraints:**
- ALL bell::Task stacks must be in internal DRAM — PSRAM stacks on Core 1 crash
  due to ESP32-S3 inter-core cache incoherency (no hardware coherency for PSRAM)
- civetweb tasks run on Core 0 to keep Core 1 free of pthreads with potential PSRAM stacks
- `bell::Task` tries `MALLOC_CAP_INTERNAL` and aborts (not falls back to PSRAM) if it fails

---

## Session Lifecycle

```
app_main()
  └─ xTaskCreateStaticPinnedToCore(cspotMainTask, core=1, stack=DRAM_ATTR[32KB])

cspotMainTask
  ├─ mdns_init() + register _spotify-connect._tcp
  ├─ BellHTTPServer(8080)              ← civetweb starts, serves /spotify_info
  │    GET  /spotify_info  → blob->buildZeroconfInfo()   (DH pubkey + metadata)
  │    POST /spotify_info  → blob->loadZeroconfQuery()   (decrypt blob, set gotBlob)
  │
  ├─ while (!gotBlob) sleep(1s)
  │
  ├─ Context::createFromBlob(blob)     ← allocates session, sets clientId/clientSecret
  ├─ session->connectWithRandomAp()   ← TCP + Shannon handshake (plain TCP, port 4070)
  ├─ session->authenticate(blob)      ← ClientResponseEncrypted → APWelcome token
  ├─ session->startTask()             ← starts MercurySession bell::Task
  ├─ SpircHandler(ctx)                ← creates TrackQueue bell::Task internally
  ├─ handler->subscribeToMercury()    ← Mercury SUB to hm://remote/3/user/{u}/
  │                                      sends kMessageTypeHello
  ├─ CSpotPlayer(handler)             ← creates cspot_player bell::Task (app level)
  └─ while(true) ctx->session->handlePacket()  ← Shannon read loop, blocks here
```

---

## Mercury Protocol

Mercury is a request/response + pub/sub layer over the Shannon-encrypted TCP connection.

### Command bytes (Shannon packet type)

| Cmd  | Name | Direction | Description |
|------|------|-----------|-------------|
| 0x02 | PING | AP → device | Keep-alive (device replies PONG 0x49) |
| 0x0D | AES_KEY | AP → device | Response to audio key request |
| 0x4A | COUNTRY_CODE | AP → device | Device's market country |
| 0xB2 | MERCURY_REQ | device → AP | Mercury GET/SEND request |
| 0xB3 | MERCURY_SUB | device → AP | Mercury subscription |
| 0xB4 | MERCURY_UNSUB | device → AP | Unsubscribe |
| 0xB5 | MERCURY_EVENT | AP → device | Pushed notification (subscription data) |

### Mercury request types

| Type | Usage |
|------|-------|
| GET | Fetch metadata, CDN URL |
| SEND | Publish Spirc frame (Notify, Hello) |
| SUB | Subscribe to Spirc control channel |

### Mercury URIs used by cspot

| URI | Purpose |
|-----|---------|
| `hm://remote/3/user/{username}/` | Spirc control channel (subscribe + send frames) |
| `hm://social-connect/v2/events/...` | Connect session events |
| `hm://metadata/3/track/{trackId}` | Track metadata (name, duration, file list) |
| `hm://audio_key/1/track/{trackId}/file/{fileId}` | Audio decryption key |

---

## Spirc Message Types

Spirc frames are protobuf-encoded (`spirc.pb`) carried inside Mercury messages.

### Device → Spotify (outbound)

| MessageType | When sent | Purpose |
|-------------|-----------|---------|
| `kMessageTypeHello` | On subscribe | Announce device is ready; contains device capabilities |
| `kMessageTypeNotify` | After state change | Broadcast current playback state to all Spotify clients |

### Spotify → Device (inbound)

| MessageType | SpircHandler action | App event fired |
|-------------|---------------------|-----------------|
| `kMessageTypeLoad` | Create new QueuedTrack; reset TrackPlayer | `PLAYBACK_START(position)`, `PLAY_PAUSE(false)` |
| `kMessageTypePlay` | Set paused=false; notify | `PLAY_PAUSE(false)` |
| `kMessageTypePause` | Set paused=true; notify | `PLAY_PAUSE(true)` |
| `kMessageTypeSeek` | seekMs(position); notify | `SEEK(position_ms)` |
| `kMessageTypeVolume` | setVolume(v); notify | `VOLUME(0–65535)` |
| `kMessageTypeNext` | | `NEXT` |
| `kMessageTypePrev` | | `PREV` |
| `kMessageTypeNotify` | Check if active device changed | `DISC` (if foreign device name) |

**Self-Notify filtering:** Mercury echoes your own SEND back to you. The Notify handler
compares `remoteFrame.device_state.name` to `ctx->config.deviceName`. If they match
it's our own echo — skip the DISC event.

---

## SpircHandler Events

The app registers a callback via `handler->setEventHandler(...)`. Event data type is
`std::variant<TrackInfo, int, bool>`.

| EventType | Data type | Value |
|-----------|-----------|-------|
| `PLAY_PAUSE` | `bool` | true=paused, false=playing |
| `VOLUME` | `int` | 0–65535 (Spotify range) |
| `DISC` | — | Another device took control |
| `FLUSH` | — | Clear audio buffer (e.g. seek) |
| `SEEK` | `int` | New position in ms |
| `PLAYBACK_START` | `int` | Start position in ms |
| `TRACK_INFO` | `TrackInfo` | Name, artist, duration, etc. |
| `NEXT` | — | Skip forward |
| `PREV` | — | Skip back |
| `DEPLETED` | — | Queue exhausted |

**Mapping to hardware (main.cpp):**
```cpp
case PLAY_PAUSE:  paused = std::get<bool>(ev->data);
case VOLUME:      audioSink->volumeChanged(std::get<int>(ev->data) >> 8);  // 0-65535 → 0-255
case DISC:        paused = true; circBuf->emptyBuffer();
case FLUSH/SEEK/PLAYBACK_START: circBuf->emptyBuffer();
```

---

## Track Data Path

```
TrackQueue (Core 1, 32 KB)
  │  Mercury: metadata → pick format (OGG_VORBIS_160 preferred)
  │  Mercury: audio key (AES-128, 16 bytes)
  │  HTTPS:   OAuth2 token (client_credentials to accounts.spotify.com)
  │  HTTPS:   CDN URL (storage-resolve API)
  ▼
TrackPlayer / CDNAudioFile (Core 1, 32 KB)
  │  HTTPS range request to CDN
  │  AES-128 CTR decrypt (key=audio key, IV from chunk index)
  │  Feed raw OGG/MP3/AAC bytes to bell codec
  ▼
bell codec (tremor for OGG, libhelix for MP3, opencore for AAC)
  │  Decode to PCM (44100 Hz, 16-bit, stereo)
  ▼
CircularBuffer (128 KB, in PSRAM)
  ▼
CSpotPlayer task (Core 0, 16 KB)  ← app-level, in main.cpp
  │  read PCM from CircularBuffer
  │  feedPCMFrames() → I2S DMA
  ▼
ES8311 codec (I2C config + I2S audio)
  ▼
NS4150 class-D amplifier → speaker
```

---

## Key Data Structures

### Context (`CSpotContext.h`)
```cpp
struct ConfigState {
    std::string deviceName;    // "SpotifyESP32"
    std::string deviceId;      // SHA1 of MAC address (hex)
    std::string clientId;      // Spotify app client_id (set by app, not committed)
    std::string clientSecret;  // Spotify app client_secret (set by app, not committed)
    int volume;                // current volume 0–65535
};
```

### SpircHandler event callback
```cpp
handler->setEventHandler([](std::unique_ptr<SpircHandler::Event> ev) {
    switch (ev->eventType) { ... }
});
```

### TrackPlayer data callback
```cpp
handler->getTrackPlayer()->setDataCallback(
    [](uint8_t* pcm, size_t bytes, std::string_view) -> size_t {
        // write PCM bytes, return bytes consumed
        return bytes;
    });
```

---

## Known Limitations

- **Switch-back after takeover:** If the Spotify app does a full Zeroconf re-auth (new POST
  to `/spotify_info`) to switch back to the device, the current implementation does not restart
  the session. The outer `cspotMainTask` loop would need to wrap the session in a restart loop
  and reset `gotBlob=false` when the session ends. If Spotify uses the existing TCP session
  (Load frame via Mercury), switch-back works correctly.

- **Volume range:** Spotify sends 0–65535; ES8311 hardware register is 0–255. Mapping via `>> 8`
  loses the bottom 8 bits of resolution but is audibly indistinguishable.

- **Single session:** No support for multiple simultaneous Spotify accounts or group sessions.
