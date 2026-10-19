# Development notes

## Goal and constraints

Receive and decode P25 trunked systems and digital voice from inside SDR++ **without forking
SDR++**. Targets are P25 Phase II systems: a Phase 1 FDMA control channel with Phase 2 TDMA voice
(and Phase 1 voice where used), including simulcast (CQPSK/LSM) sites. Some sites span 3.7–5 MHz,
wider than a 2.4 MS/s RTL-SDR window, so single-tuner retune-per-call is required. Encrypted
talkgroups must be skipped and never decrypted. No keys are ever configured, and the module
never sends `key_*`, `decryption_*` or `force_*` API commands.

Repo hygiene: the code and examples must not contain real system names, sites, frequencies,
locations, or anything about how the code was written. Use the fictional "Example System" on
851–856 MHz. Users' own playlists and profiles live in `~/.config/sdrpp/p25_trunking/`, and
`.gitignore` excludes `*.xml` outside `examples/`.

## Design decision: dsd-neo sidecar

Options considered:

| Option | Verdict |
|---|---|
| Write a P25 decoder as native SDR++ blocks (port OP25/dsd-neo DSP + mbelib-neo) | Weeks of work. Simulcast CQPSK and Phase 2 TDMA are the hard parts. Rejected. |
| Link dsd-neo's libraries in-process | dsd-neo has global state and still-changing internal APIs, and a decoder crash would take SDR++ down. Rejected. |
| **Run dsd-neo as a separate process and feed it IQ through a virtual rtl_tcp tuner** | Chosen: no decoder code to maintain, dsd-neo updates independently, clean GPL boundary, crash isolation. |

Projects evaluated (October 2026):
- **dsd-neo** (arancormonk, GPL-3.0, very active; successor to DSD-FME). Has IQ-domain CQPSK
  (Gardner + Costas), an explicit P25 trunking state machine, Phase 1/2, an API and UDP audio.
  Its `legal/status.md` lists a few files with unclear provenance; they're not our concern,
  since we run it as a separate program.
- **mbelib-neo** (GPL-2.0+): IMBE + AMBE+2. Bundled in dsd-neo's AppImage (2.6.2).
- **DSD-FME:** mostly ISC, with some GPL-2-only D-STAR files. Its CQPSK slices the FM
  discriminator, which is weak on simulcast. Superseded by dsd-neo.
- **OP25** (boatbod, GPL-3+): good DSP, but GNU Radio-bound C++ with Python trunking logic.
  Useful as a reference.
- **SDRTrunk** (Java, GPL-3): the best simulcast handling. Reference only.
- **cropinghigh/sdrpp-vhfvoiceradio:** an SDR++ module with old DSD and P25 Phase 1 voice only.
  No trunking and no Phase 2; last updated 2024.
- **dsdcc:** no P25 framing. Not useful.

## Layout

- `src/main.cpp`: module entry points; lifecycle (start/stop, resume via `wasRunning`, sink
  default via `sinkDefaulted`); VFO + IQ handler; `tuneTo` (retunes); decoder command line;
  telemetry state; ImGui panel.
- `src/retune_policy.h`: pure function choosing the SDR centre for a target (header-only,
  unit-tested). Usable half-window = 0.42·fs, DC guard 60 kHz. Order: stay put → "home" centre
  covering every known site frequency → centre covering target + control channel → target
  ±250 kHz towards the control channel.
- `src/iq_ring.h`: cf32 → cu8 (`v·127.5·gain + 127.5`), bounded ring (drop-oldest, overrun
  counter), flush with 40 ms mid-scale padding after a hardware retune, level/clip meter.
- `src/rtltcp_server.*`: loopback rtl_tcp server (RTL0 header, tuner=E4000, 0 gains). Handles
  0x01 freq and 0x02 rate and ignores the rest. One client at a time; sends 64 KiB chunks with
  TCP_NODELAY.
- `src/sidecar.*`: decoder process in its own process group, `PR_SET_PDEATHSIG`, stdout/stderr
  to a 500-line tail and a rotating log, restart backoff 1/2/5/10/30 s (gives up after 5 exits
  in 2 min), stop = SIGTERM to the group then SIGKILL.
- `src/audio_rx.*`: UDP voice → FIFO → player paced by the sink (see "Audio" below).
- `src/api_client.*`: JSON-Lines client (welcome → subscribe → `get status`; reconnects).
- `src/profiles.*`: site profiles (JSON) and the generated channel map (`run/<id>_chan.csv`).
- `scripts/`: `fetch_sdrpp.sh` (SDR++ source at `SDRPP_COMMIT`), `register_module.py`,
  `import_sdrtrunk.py`.
- `tests/`: retune unit test, fake decoder rtl_tcp client, fake API server, UDP tone sender.

Runtime files: `~/.config/sdrpp/p25_trunking_config.json` (module settings: decoder path,
ports, gain, volume, audio mode, profile, launchDecoder, wasRunning, sinkDefaulted) and
`~/.config/sdrpp/p25_trunking/{profiles,groups,run}`.

## Build and install

```sh
scripts/fetch_sdrpp.sh && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake --install build --prefix <prefix>    # only with SDR++ closed
```

- The build uses cropinghigh's out-of-tree approach: SDR++ core source as headers
  (`src/`, `src/imgui/`), linking the installed `/usr/lib/libsdrpp_core.so`, with `-std=c++17`.
  `SDRPP_COMMIT` = 8c9f5ee8fe405775bfcd62c8c8f8c0fc928a64af, which is nightly package
  `1.2.1-1517` (reports v1.3.0). `1517` is the "Build Binaries" workflow run number. No ABI
  stability: rebuild after every SDR++ upgrade.
- SDR++'s README warns that *distribution* `sdrpp` packages don't work with out-of-tree
  modules. The official nightly `.deb` does.
- `config.json` `"modules"` accepts absolute .so paths, so no install into `/usr/lib` is needed.
- Never overwrite the loaded .so while SDR++ runs, since that can crash it. SDR++ rewrites
  `config.json` and module configs on exit, so edit them only while it's closed.

## Decoder facts (dsd-neo 2.11.4, verified live)

- **rtl_tcp client:** reads the 12-byte header and ignores the tuner type. At connect it sends
  0x03 gain mode, 0x08 AGC, 0x0A offset tuning, 0x01 frequency, then 0x02 rate. The rate is
  **1,536,000 S/s** for DSP bandwidth 48 (the `bw` field of
  `-i rtltcp:host:port:freq:gain:ppm:bw:sql:vol`). The TCP backend never reads back an actual
  rate, so it must be exact.
- **Environment variables the module sets:**
  - `DSD_NEO_DISABLE_FS4_SHIFT=1`: makes 0x01 carry the channel frequency (otherwise channel +
    rate/4).
  - `DSD_NEO_TCP_PREBUF_MS=100`: the default 1000 ms network prebuffer only delays decoding on
    loopback.
  - `DSD_NEO_NO_BOOTSTRAP=1`: skips the interactive first-run setup.
- **Command line:** `-ft [-mq] -^ -T --enc-lockout --tg-lockout-session --frontend none`, plus
  `-i rtltcp:…`, `-C` channel map, `-G` group CSV, `-o udp:127.0.0.1:23456`, `--api 9911`,
  `-J` event log.
  - `-ft` must precede `-mq`.
  - `-mq` is for simulcast.
  - `-^` prefers control-channel candidates during the hunt.
  - **`--enc-lockout` is mandatory**: the default `--enc-follow` follows encrypted grants.
- **Channel map:** header `ChannelNumber(dec),frequency(Hz),note`, used as the control-channel
  hunt list. P25 voice frequencies come from IDEN_UP broadcasts. Grants are refused ("missing
  iden params") for the first few seconds until those arrive; that's normal.
- **Group CSV:** header `id,mode,name,tag`, no quoting, ranges `a-b` allowed. Mode A follows; B
  and DE lock out.
- **UDP voice with `-ft`:** 8 kHz s16le **stereo** (L = slot 1, R = slot 2), 640-byte datagrams
  (20 ms). Phase 2 arrives in **~120 ms bursts with gaps up to ~125 ms**; the average rate is
  real time or slightly above.
- **API:** `{"type":"welcome",...}` first. Requests are `{"id","cmd",...}`. Telemetry objects
  carry `type` (call/event/system/status/metrics/quality/result), and responses echo `id`.
  Telemetry is sent at most every 250 ms. Numeric fields can be `null`, so use the `num()`
  helper. Useful commands: `tg_hold_set {value}`, `lockout_slot {value:slot}`,
  `skip_slot {value:slot}`, `tg_listen_set {id_start,id_end,listen}`, `return_cc`, `quit`.
  The API accepts several clients, so external tools can query it alongside the module.
- **AppImage:** `AppRun` resolves paths from `$0`, so a symlink to it fails
  (`apprun-hooks/... not found`). Use a wrapper script that `exec`s `squashfs-root/AppRun`.
  `AppRun` itself `exec`s the binary, so it's one process and signals reach the decoder.

## SDR++ facts (commit 8c9f5ee)

- **VFO:** `createVFO(name, REF_CENTER, offset, bw, rate, minBw, maxBw, locked)`. With
  bw == rate, `RxVFO` skips its filter, which is cheap. The module's VFO is 1.536 MHz wide on
  the waterfall.
- **Retune from a non-GUI thread:** `gui::waterfall.setCenterFrequency(c)`,
  `sigpath::sourceManager.tune(c)`, then `sigpath::vfoManager.setOffset(name, f − c)`. This
  follows rigctl_server's precedent of tuning from its network thread. Not `tuner::tune`, which
  applies the user's centre-tuning mode.
- **`sourceManager.onRetune`** fires for every retune, including the module's own (hence the
  `ownRetune` flag). The module re-places its tuning only while the decoder is connected
  (`server.clientConnected`). Otherwise it fights other tools, for example an external sweep.
- **Audio sink:** `audio_sink` pulls `sampleRate/60`-sample blocks in an RtAudio callback that
  **blocks** on its input. A producer paced by its own timer makes that callback wait, which
  sounds choppy. The player therefore has no timer: `out.swap()` backpressure through
  resampler → splitter → volume → packer paces it to the sound card, and the next block is
  always ready (silence when idle). Each call is prebuffered 250 ms (longer than one burst plus
  gap). Within a call, shortfalls are padded with silence. The player goes idle after 400 ms
  without packets; a short transmission that has ended plays immediately.
- **New sink streams default to sink "None".** `postInit` sets "Audio" once
  (`sinkDefaulted`), after which the user's choice in the Sinks menu wins. The Radio stream
  keeps playing whatever its VFO is on, usually static, so users should mute it.

## Verification done

- **Retune policy:** unit tests pass (a fitting site never retunes after home; on a wide site,
  every target is usable and nearby voice keeps the control channel in view; DC-spike
  avoidance).
- **rtl_tcp link** (fake decoder client against TV pilots at lower edge + 309.441 kHz): header
  OK, steady-state rate 1.5364 MS/s, pilots within the dongle's ppm error (about −310 Hz at
  ~500 MHz), and the tone appears about 60 ms after a hardware retune. The test's rate figure
  is inflated right after a retune, because it includes queued data.
- **Live decode on a P25 Phase II simulcast site** (CQPSK, `-mq`):
  - control channel locked after the hunt and never lost over 90 s;
  - 100% control-channel FEC (653/653);
  - WACN/SYS/RFSS/site decoded;
  - Phase 2 voice followed on both slots with talkgroup names from the CSV and talker
    aliases;
  - 0 voice errors at ~20 dB SNR;
  - encrypted grant updates seen and not followed;
  - 13 neighbour sites learned.
- **Audio:** the raw decoder output (UDP capture) was continuous speech. The choppiness was in
  the player, and was fixed by sink-clock pacing.
- **Lifecycle:** decoder killed with SDR++ (no orphans), resumes at the next SDR++ start, API
  reachable by a second client.

## Troubleshooting runbook

1. **Is the SDR streaming?** The panel's IQ level should be around −20 to −40 dBFS. If the
   receiver was unplugged and replugged (its USB device number changes), SDR++ must be
   restarted: it only scans for devices at startup.
2. **Is there a control channel to decode?** Look for a steady 12.5 kHz carrier at the site's
   frequencies. Low RF gain is the usual cause of `cc-lost` hunting loops: a gain step that
   suited a strong local signal can be far too low for distant towers.
3. **Decoder log:** `~/.config/sdrpp/p25_trunking/run/decoder.log` (ANSI colours; strip with
   `sed 's/\x1b\[[0-9;]*m//g'`). Look for `Sync: +P25p1 ... NAC`, `[P25 SM]` state changes,
   `Retune applied`, and Phase 2 `4V`/`2V` voice frames.
4. **Query the API as a second client:** `get` `system` / `status` / `quality` / `metrics`
   (cc_fec ok/err, p25p2_voice_err, snr_db).
5. **Audio:** check the Sinks menu routing first. To separate decoder problems from player
   problems, capture UDP 23456 with a second `SO_REUSEADDR` socket (this steals the packets
   while it runs) and write a WAV.
6. **To debug the decoder by hand,** untick Launch dsd-neo, press Start, and run the command
   the panel shows.

## Ideas not done yet

- A second-tuner mode (separate SDR++ instance or source) so the control channel stays
  monitored during calls on wide sites.
- Persist panel lockouts to the group CSV, and add per-talkgroup priority.
- A narrower VFO display, since the 1.536 MHz box dominates the waterfall. Show IQ overruns and
  decode quality history; record calls to WAV.
- Test the panel's "Mute TG" (`tg_listen_set`) and the hold/lockout/skip buttons on live
  traffic.
