# Development notes

## Layout

- `src/main.cpp`: module entry points, lifecycle (start/stop/resume), VFO + IQ handler,
  retuning (`tuneTo`), decoder command line, telemetry state, ImGui panel.
- `src/retune_policy.h`: pure function choosing the SDR centre for a target frequency
  (header-only; unit-tested in `tests/test_retune_policy.cpp`).
- `src/iq_ring.h`: cf32 → cu8 conversion and the bounded ring between SDR++'s DSP thread and
  the rtl_tcp sender (drop-oldest, overrun counter, flush with mid-scale padding).
- `src/rtltcp_server.*`: loopback rtl_tcp server (RTL0 header, 0x01 freq / 0x02 rate handled,
  other commands ignored).
- `src/sidecar.*`: decoder process (own process group, PDEATHSIG, log capture/rotation,
  restart with backoff).
- `src/audio_rx.*`: UDP voice → FIFO → player paced by the audio sink (see below).
- `src/api_client.*`: decoder JSON-Lines API client (welcome → subscribe → telemetry; commands).
- `src/profiles.*`: site profiles (JSON) and the generated channel map.
- `scripts/`: SDR++ source fetch, module registration, SDRTrunk import.
- `tests/`: retune unit test, fake decoder rtl_tcp client, fake API server, UDP tone sender.

## Build

```sh
scripts/fetch_sdrpp.sh && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake --install build --prefix <prefix>    # never overwrite the .so while SDR++ has it loaded
```

Install only with SDR++ closed: replacing a loaded plugin in place can crash SDR++. SDR++
rewrites `config.json` and module configs on exit, so edit them only while it's closed.

## Decoder facts (dsd-neo 2.11.4, verified)

- rtl_tcp client: reads the 12-byte header and ignores the tuner type. Sends, in order: 0x03 gain
  mode, 0x08 AGC, 0x0A offset tuning, 0x01 frequency, 0x02 rate. The rate is 1,536,000 S/s for a
  48 kHz DSP bandwidth (`bw` field of `-i rtltcp:host:port:freq:gain:ppm:bw:sql:vol`).
- Environment variables passed to the decoder:
  - `DSD_NEO_DISABLE_FS4_SHIFT=1` makes 0x01 carry the channel frequency; otherwise it's offset
    by rate/4.
  - `DSD_NEO_TCP_PREBUF_MS=100`: the default 1 s network prebuffer only adds latency on loopback.
  - `DSD_NEO_NO_BOOTSTRAP=1` skips the interactive first-run setup.
- Always pass `--enc-lockout`: the default follows encrypted grants. Never use key/decryption
  commands or options.
- UDP voice with `-ft`: 8 kHz s16le **stereo** (L = slot 1, R = slot 2), 640-byte datagrams
  (20 ms). Phase 2 arrives in ~120 ms bursts with gaps up to ~125 ms; the average is real time.
- API: `{"type":"welcome"}` first. Telemetry objects have `type` (call/event/system/status/
  metrics/quality); responses echo `id`. Numeric fields can be `null` (use the `num()` helper).
- The AppImage's `AppRun` resolves paths from `$0`, so a symlink to it breaks. Use a wrapper
  script that `exec`s `squashfs-root/AppRun` (a single process, so signals reach the decoder).

## SDR++ facts (commit 8c9f5ee)

- `config.json` `"modules"` takes absolute .so paths, so no install into `/usr/lib` is needed.
- The audio sink pulls 1/60 s blocks in an RtAudio callback that blocks on its input. A source
  that paces itself with its own timer makes that callback wait, which sounds choppy. Let
  `stream.swap()` backpressure pace the producer, and always have the next block ready
  (silence when idle).
- New sink streams default to sink "None". The module sets "Audio" once (`sinkDefaulted`).
- `sourceManager.onRetune` fires for every retune, including the module's own (`ownRetune` flag).
  The module re-places its tuning only while the decoder is connected, or it would fight other
  tools.
- `tuner::tune` honours the user's centre-tuning mode. The module uses
  `waterfall.setCenterFrequency` + `sourceManager.tune` + `vfoManager.setOffset` directly.

## Testing

No CI. Before changing the IQ/tuning path, run `tests/fake_dsdneo_client.py` against steady
carriers (TV pilots: channel lower edge + 309.441 kHz) with Launch dsd-neo off. Expect a peak
within the dongle's ppm error, a delivered rate of 1.536 MS/s, and the tone ≤ 100 ms after a
retune. For decoding, check the decoder log (`~/.config/sdrpp/p25_trunking/run/decoder.log`)
and query the API (`get` `system` / `quality`) as a second client.

## Ideas not done yet

- A second-tuner mode (separate SDR++ instance or source) so the control channel stays
  monitored during calls on wide sites.
- Per-talkgroup priority and persistence of panel lockouts to the group CSV.
- Show IQ overruns and decode quality history in the panel; record calls to WAV.
