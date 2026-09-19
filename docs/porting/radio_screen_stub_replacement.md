# radio_screen Stub Replacement Plan

## Current State

`radio_screen.cxx` (1257 lines) was ported from MetalioClaw4's `radio_screen.cc`
(1770 lines). The UI, layout, event handlers, and lifecycle management are
preserved verbatim. However, the audio pipeline is stubbed because the required
ESP-IDF components are not yet available on openvela/NuttX.

Audio verification on hardware (2026-08-11) confirmed:
- **Speaker output: VERIFIED** — 440 Hz tone plays through `/dev/audio/pcm0`,
  `AUDIO_MSG_COMPLETE` received, `MetalioAudioCodec` + TCA9555 PA path works.
- **Microphone capture: software path works, hardware timeout** — TX silence
  buffer generates BCLK/WS, RX DMA starts (`I2S_RECEIVE=0`), but no data
  captured within 3 s. Likely no mic signal on GPIO 11 (DIN) — hardware/wiring
  issue, not a software blocker.

So the I2S output path that a real HLS player would feed is already proven.
The only missing piece is the HLS → PCM decode pipeline.

## Stubs to Replace

### 1. RadioPlayerStub (radio_screen.cxx:367-408 — `RadioPlayTask`)

**Replaces:** `esp_audio_simple_player_run_to_end()` + `esp_hls_io` +
`esp_gmf_audio_dec` pipeline.

**Current behavior:** Simulates `Connecting → Playing` state machine with a
~1.2 s delay. Does not produce any real audio output.

**What needs to be ported (dependency tree):**

```
esp_audio_simple_player (espressif/esp_audio_simple_player ^0.9.4~1, ~280 KB)
├── esp_gmf core (pipeline / pool / element / event)
│   ├── esp_gmf_oal (OS abstraction — FreeRTOS task/mutex/queue)
│   └── esp_gmf_chunk (ring-buffer I/O between elements)
├── esp_audio_simple_dec (format detection + decoder dispatch)
│   ├── esp_audio_codec (AAC/MP3/FLAC/WAV decoder cores — **binary libs**)
│   └── esp_codec_dev (codec device abstraction)
├── esp_gmf_audio_dec (GMF element wrapping esp_audio_simple_dec)
├── esp_gmf_io_http (HTTP IO element — wraps esp_http_client)
│   └── esp_http_client (ESP-IDF native, not in esp-hal-3rdparty)
├── esp_gmf_rate_cvt / bit_cvt / ch_cvt (audio transformers)
└── esp_hls_stream (espressif/esp_hls_stream ==1.0.2, ~287 KB)
    ├── esp_extractor (container demuxer framework — TS, ES)
    ├── media_lib_sal (media lib OS abstraction)
    └── gmf_io (GMF I/O types)
```

### 2. SpectrumTask (radio_screen.cxx:307-351 — xorshift32 PRNG)

**Replaces:** `esp_gmf_fft` real-FFT pipeline (`gmf_fft: ^1.0.0`).

**Current behavior:** Uses `xorshift32` PRNG to generate plausible-looking but
fake spectrum data.

`gmf_fft` is the smallest, most self-contained of the bunch — a Q15 fixed-point
real FFT. It does not depend on `esp_http_client` or `esp_codec_dev`. Porting
just `gmf_fft` is a tractable ~1-2 day task.

## Feasibility Assessment (2026-08-11)

### Component availability

| Component | In esp-hal-3rdparty? | On components.espressif.com? |
|---|---|---|
| `esp_audio_simple_player` | ❌ No (94 components, none audio-streaming) | ✅ Yes (~280 KB archive) |
| `esp_hls_stream` | ❌ No | ✅ Yes (~287 KB archive) |
| `gmf_audio` / `gmf_io` / `gmf_fft` | ❌ No | ✅ Yes |
| `esp_codec_dev` (AAC/MP3 cores) | ❌ No | ✅ Yes — **binary libs, ESP-IDF toolchain** |
| `esp_http_client` | ❌ No (NuttX has `webclient` / `libcurl4nx`) | Part of ESP-IDF |
| `esp_extractor` / `media_lib_sal` | ❌ No | ✅ Yes |

### Network access (verified 2026-08-11)

- `https://components.espressif.com` → ✅ reachable directly (HTTP 200).
- `https://raw.githubusercontent.com` → ✅ reachable (301 → github.com).
- Old proxy `127.0.0.1:7890` not required for these hosts.

So the source archives **can** be downloaded. The blocker is not network
access — it is the porting effort itself.

### Blockers for a direct port to openvela/NuttX

1. **ESP-IDF framework dependency.** Every component is built on ESP-IDF's
   `esp_event` loop, `esp_http_client`, `esp_log`, and FreeRTOS APIs.
   openvela has shims for `esp_log` and FreeRTOS (`freertos_shim.h`) but
   **no shim for `esp_event` or `esp_http_client`**. An adapter layer would
   have to be written.

2. **Binary codec libraries.** `esp_audio_codec` (the actual AAC/MP3/FLAC
   decoders) ships as precompiled `.a` archives for the ESP-IDF toolchain.
   They link with `riscv32-esp-elf-gcc` (which the project already uses), but
   they expect ESP-IDF's libc/syscall surface, not NuttX's. Whether they link
   cleanly under NuttX is **unverified** and likely needs symbol shims.

3. **ESP-GMF architecture.** GMF is a pipeline framework with its own task
   pool, chunk ring buffers, and event system. Porting it requires either:
   - replacing the entire framework with NuttX-native equivalents, or
   - running the GMF stack on compatibility shims (`esp_gmf_oal` already
     abstracts the OS layer — this is the most promising path).

4. **Build system.** ESP-IDF uses CMake + `idf_component.yml`; NuttX uses
   Make/CMake + `Kconfig` + `Make.defs`. Each component needs build-system
   adaptation (similar to how `esp-hal-3rdparty` is wired today).

5. **HTTP client API mismatch.** GMF's `esp_gmf_io_http` wraps
   `esp_http_client`. NuttX `webclient` / `libcurl4nx` have different APIs;
   an adapter would be needed.

### Effort estimate

| Path | Scope | Effort |
|---|---|---|
| **A. Full `esp_audio` + `esp_hls` + `gmf` port** | Port ~6 components + adapters + binary codec libs + build wiring | 2-4 weeks (multi-week, deep ESP-IDF + NuttX expertise) |
| **B. Port `gmf_fft` only** | Self-contained Q15 FFT, replace `SpectrumTask` stub | 1-2 days |
| **C. Minimal HTTP MP3 player (interim)** | `webclient` + `libmad` + `MetalioAudioCodec`; no HLS, no AAC | 2-3 days |
| **D. Minimal HLS-MP3 player (interim)** | Custom m3u8 parser + MPEG-TS demuxer + `libmad`; no AAC | ~1 week |
| **E. Keep stub, wait for full port** | No code change; document dependency | 0 |

## Recommendation

The user's task statement is explicitly conditional: **"当 esp_audio / esp_gmf
移植到 openvela 后，替换 radio_screen 的 stub HLS 播放器"**. The porting has
not happened, and a full port (path A) is a multi-week effort that should be
tracked as a separate epic, not done inline.

For the current session, the realistic options are:

1. **Path B (port `gmf_fft`)** — small, self-contained win. Replaces the
   `xorshift32` PRNG in `SpectrumTask` with a real Hann-windowed FFT running
   on the (still stubbed) PCM. Visualizer becomes correct once real PCM
   arrives. Low risk, doesn't unblock audio.

2. **Path C (minimal HTTP MP3 player)** — gives real audio for stations that
   publish direct MP3 streams, but **does not work with the current
   `radio_stations.h` URLs** (they are all `.m3u8`). Would require maintaining
   a separate MP3-URL station list. Useful as a proof-of-concept of the
   NuttX audio-output path for streaming audio (which the speaker test
   already proves for tone generation).

3. **Path E (keep stub)** — the stub already works for UI demonstration; the
   real port is the user's stated precondition. Update the plan doc and stop.

## Original stub-replacement notes (preserved for the future port)

### ESP-IDF → NuttX API mapping (for when the port happens)

| ESP-IDF API | Purpose | NuttX/openvela Equivalent |
|---|---|---|
| `esp_audio_simple_player_run_to_end()` | Stream HLS m3u8 URL → decode → PCM | NuttX `/dev/audio/pcm0` + custom HLS fetcher |
| `esp_hls_io` | HTTP Live Streaming I/O adapter | `libcurl4nx` or `webclient` + m3u8 parser |
| `esp_gmf_audio_dec` | Audio decoder (AAC/MP3) | `esp_codec_dev` binary libs OR `libmad` (MP3 only) |
| `AudioCodec::OutputData()` | PCM output callback | `MetalioAudioCodec::WriteAudio()` (already exists) |
| `esp_audio_simple_player_stop()` | Stop playback | `ioctl(AUDIOIOC_STOP)` |
| `esp_gmf_fft` | Real-to-complex FFT on PCM | Port `gmf_fft` (path B) or use `kissfft` |
| `portMUX_TYPE` | Critical section | `std::mutex` (already done) |
| `SemaphoreHandle_t` | Binary sem | `std::mutex` (already done) |
| `TaskHandle_t` | FreeRTOS task | `std::thread` (already done) |
| `AudioCodec` class | Output volume | `AudioService::GetVolume/SetVolume` (already done) |
| `vTaskDelete(nullptr)` | Self-terminate | `task_delete(getpid())` |

### Replacement steps (for when the port happens)

1. Port `esp_audio_simple_player` from esp-hal-3rdparty (or implement a
   NuttX-native audio player that reads from an HLS URL).
2. Port `esp_hls_io` — HTTP Live Streaming I/O adapter. Requires:
   - HTTP client (`apps/netutils/webclient` or `libcurl4nx`)
   - m3u8 parser (port from `esp_hls_stream`)
   - Segment download + buffering
3. Port `esp_gmf_audio_dec` — audio decoder pipeline. Requires:
   - AAC/MP3 decoder (port `esp_codec_dev` binary libs OR use `libmad` for MP3)
   - PCM resampling (`apps/audioutils/speexdsp`)
4. Port `gmf_fft` — replace `SpectrumTask` PRNG with real FFT (path B above).
5. Replace `RadioPlayerStub` class with real `RadioPlayer` that:
   - Opens the HLS URL
   - Starts the decode pipeline
   - Calls `AudioService::WriteAudio()` / `MetalioAudioCodec` with decoded PCM
   - Manages playback state (Connecting → Playing → Stopped)

### Testing (after the real port)

1. Select a station → verify "Connecting..." appears
2. After ~1s → verify "Playing" state + audio output
3. Verify spectrum bars respond to actual audio frequency content
4. Stop → verify audio stops and state returns to idle
5. Switch stations → verify clean transition
