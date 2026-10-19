# sdrpp-p25

An [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) module for **P25 trunking and digital
voice**. It follows a P25 Phase 1 control channel, tunes to Phase 1 (FDMA) and Phase 2 (TDMA)
voice grants, and plays the decoded voice through SDR++'s own audio output. Simulcast (CQPSK)
sites are supported. Encrypted calls are detected and skipped. It is an out-of-tree plugin:
SDR++ itself is not modified or forked.

## How it works

SDR++ has no P25 support, and no SDR++ module did trunking. Rather than re-implement a P25
decoder inside SDR++, this module pairs SDR++ with
[dsd-neo](https://github.com/arancormonk/dsd-neo), an actively maintained decoder. dsd-neo
handles P25 Phase 1/2 trunking, IQ-domain CQPSK demodulation for simulcast, encrypted-call
lockout, and IMBE/AMBE+2 voice (via mbelib-neo).

dsd-neo can drive an `rtl_tcp` tuner itself, so the module **pretends to be one**:

```
SDR++ ── RTL/Airspy/... source ──┬── your Radio VFO, other modules (unchanged)
                                 └── "P25 Trunking" VFO ── rtl_tcp server (127.0.0.1:14423)
                                                               │  IQ ▲ retune requests
                                                           dsd-neo (trunking, demod, vocoder)
          SDR++ audio sink ◄── player ◄────── UDP voice ───────┘
          module panel     ◄── JSON-Lines API (calls, system, metrics)
```

1. **IQ.** The module creates its own VFO in SDR++ and serves its samples to dsd-neo as an
   `rtl_tcp` stream (8-bit IQ at the 1.536 MS/s dsd-neo asks for).
2. **Retuning.** When dsd-neo asks the "tuner" for a frequency, for example a voice grant, the
   module moves its VFO. If the target is outside the SDR's current bandwidth, it recentres the
   SDR. A site whose channels all fit in the SDR bandwidth gets one fixed "home" centre, so it
   never retunes the hardware.
3. **Voice.** dsd-neo sends decoded 8 kHz stereo audio (slot 1 left, slot 2 right) over UDP. The
   module buffers it and plays it into SDR++'s audio sink system as a stream called
   *P25 Trunking*.
4. **Panel.** The module reads dsd-neo's JSON-Lines API for system and site identity, current
   calls, recent calls and decode quality. It sends API commands for hold, lock out and skip.
5. **Lifecycle.** dsd-neo runs as a child process. It is restarted if it crashes, stopped with
   the module or SDR++, and started again automatically when SDR++ starts if it was running at
   exit.

Because SDR++ still owns the radio, the waterfall, other VFOs and other modules keep working
while the module trunks.

## Features

- **P25 Phase 1 control channel, Phase 1 and Phase 2 voice**, with talkgroup and radio
  aliases, emergency flags and talker aliases where the system sends them.
- **Simulcast (CQPSK/LSM) and C4FM sites.**
- **Single-tuner trunking**, with a fixed centre for sites that fit the SDR bandwidth and
  retune-per-call for wider sites.
- **Encrypted calls skipped**, never decrypted, and shown as such in the call list.
- **Panel in SDR++:**
  - site profile picker, start and stop;
  - system and site identity, NAC, control and voice frequencies, SNR;
  - slot 1 and slot 2 calls with hold, lock out and skip;
  - recent calls with per-talkgroup mute;
  - volume and slot routing, IQ level and buffer health;
  - decoder log.
- **SDRTrunk playlist import:** site frequencies and modulation become profiles, alias lists
  become talkgroup lists, and "do not monitor" talkgroups are locked out.

## Install

### 1. Requirements

- **Linux**, and **SDR++ 1.3 (nightly) installed from the official `.deb`** from SDR++'s GitHub
  releases. Distribution-built `sdrpp` packages are not compatible with out-of-tree modules.
- **A receiver SDR++ can use.** Development used an RTL-SDR at 2.4 MS/s.
- **Build tools and SDR++'s build dependencies:**
  ```sh
  sudo apt install git cmake g++ libvolk-dev libfftw3-dev libglfw3-dev libzstd-dev
  ```

### 2. dsd-neo

Download `dsd-neo-linux-x86_64-portable-<version>.AppImage` (2.11.4 or later) from
[dsd-neo's releases](https://github.com/arancormonk/dsd-neo/releases). It includes mbelib-neo.

Extract it once and use a small wrapper script. This avoids needing FUSE, and it keeps the
decoder a single process, so it stops cleanly with SDR++:

```sh
mkdir -p ~/tools/dsd-neo && cd ~/tools/dsd-neo
chmod +x dsd-neo-linux-x86_64-portable-*.AppImage
./dsd-neo-linux-x86_64-portable-*.AppImage --appimage-extract >/dev/null
printf '#!/bin/sh\nexec "%s/squashfs-root/AppRun" "$@"\n' "$PWD" > dsd-neo && chmod +x dsd-neo
./dsd-neo -h | head -3      # should print dsd-neo's usage
```

Use a wrapper rather than a symlink: `AppRun` locates its files from its own path, so a
symlink breaks it. The module looks for `dsd-neo` in its configured path, then `~/.local/bin`,
`/usr/local/bin` and `/usr/bin`. You can set the path in the module's Settings.

### 3. Build the module

SDR++ has no stable module ABI. The module must be compiled against the core source of the
**exact commit** your SDR++ build came from, named in `SDRPP_COMMIT`.

```sh
git clone https://github.com/entitledosprey/sdrpp-p25.git && cd sdrpp-p25
scripts/fetch_sdrpp.sh                         # SDR++ source at SDRPP_COMMIT → third_party/
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

To find the commit for a different SDR++ build: the nightly package version is
`1.2.1-<run>`, where `<run>` is the run number of SDR++'s "Build Binaries" GitHub workflow.
That run names the commit. Put it in `SDRPP_COMMIT` and run `scripts/fetch_sdrpp.sh` again.

### 4. Install and register (SDR++ closed)

```sh
cmake --install build --prefix ~/.local       # → ~/.local/lib/sdrpp/plugins/p25_trunking.so
python3 scripts/register_module.py ~/.local/lib/sdrpp/plugins/p25_trunking.so
```

Close SDR++ before installing or registering. Overwriting a plugin SDR++ has loaded can crash it,
and SDR++ rewrites its config files when it exits, which would undo the registration.
`register_module.py` backs up `config.json` first.

### 5. Add a site profile

Profiles are JSON files in `~/.config/sdrpp/p25_trunking/profiles/`, one per site. To create
them from an SDRTrunk playlist:

```sh
python3 scripts/import_sdrtrunk.py my_playlist.xml
```

Or write one by hand (see `examples/example_profile.json`):

```json
{
  "name": "Example System - North Site",
  "system": "Example System",
  "modulation": "C4FM",
  "control_channels": [851012500, 851512500],
  "voice_frequencies": [],
  "group_csv": "groups/Example_System.csv"
}
```

- **`modulation`:** `CQPSK` for simulcast sites, `C4FM` otherwise.
- **`control_channels`:** the hunt list. Listing all of a site's frequencies is fine, because
  the decoder finds the live control channel itself.
- **`voice_frequencies`:** filled in automatically as calls are heard. It helps choose a
  centre that covers the whole site.
- **`group_csv`:** a [dsd-neo group list](https://github.com/arancormonk/dsd-neo/blob/main/docs/csv-formats.md),
  `id,mode,name,tag`. Mode `A` follows the talkgroup and `B` locks it out. The path is relative
  to `~/.config/sdrpp/p25_trunking/`.

### 6. First run

1. Start SDR++ and make sure the source is playing.
2. Open the **P25 Trunking** panel and pick a profile. Under **Settings**, check that the
   dsd-neo path says "using …", then press **Start**.
3. Open SDR++'s **Sinks** menu. The **P25 Trunking** stream should go to your audio device; the
   module sets this the first time. **Mute the Radio stream** unless you want to hear both.
4. Within a few seconds the panel should show the protocol, system/site and control channel,
   and calls as they are granted.

## Using it

- **Slot buttons:** **Hold** stays on that talkgroup, **Lock out** stops following it, and
  **Skip** leaves the current call. **Release hold** and **Back to control channel** are below
  the slots.
- **Recent calls:** **Mute TG** stops following a talkgroup for the session. Encrypted calls
  are listed in red as skipped.
- **Audio:** volume, mute, and routing (slots left/right, mono mix, or one slot only).
- **Tuning:** while running, the module owns the SDR's tuning. If something else retunes SDR++
  while the decoder is connected, the module moves it back, so use a second receiver for other
  listening.
- **Settings:** untick **Launch dsd-neo** to run the decoder yourself, which is useful for
  debugging. The panel then shows the exact command line.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Panel stays on "IQ link: waiting" | dsd-neo isn't running or connecting: check the decoder log in the panel and the dsd-neo path in Settings. |
| Decoder hunts forever (`cc-lost` in the log) | No control channel is being received. Check a site frequency on the waterfall: a control channel is a steady 12.5 kHz carrier that never stops. Raise the RF gain (about 30–40 dB on an RTL-SDR), use an antenna suited to the band, and check the profile's modulation (CQPSK for simulcast). |
| Static instead of voice | You're hearing SDR++'s Radio stream. In **Sinks**, route **P25 Trunking** to your audio device and mute **Radio**. |
| Choppy voice | Check the "audio underruns" counter. If it climbs during calls, look for CPU overload or IQ buffer overruns (shown in the panel). Calls normally start about 0.25 s late; that buffer is deliberate. |
| Encrypted calls are silent | By design: they're skipped and shown as ENC. |
| No IQ at all after replugging the SDR | SDR++ scans for devices at startup: restart SDR++ after reconnecting the receiver. |
| SDR++ crashes or the module fails to load after an SDR++ update | Rebuild against the new SDR++ commit (see step 3). |

The decoder log is in `~/.config/sdrpp/p25_trunking/run/decoder.log`. A per-site call log is
written to `<profile>_events.log` in the same directory.

## Limits

- **One tuner:** on a site wider than the SDR bandwidth, the control channel isn't monitored
  during a call, so calls on other talkgroups are missed until it returns.
- **Reception:** simulcast decoding depends on reception more than anything else. The panel
  shows SNR and control-channel decode health.
- **Ports used on 127.0.0.1:** 14423 (rtl_tcp), 9911 (decoder API), 23456 (voice). They can be
  changed in `~/.config/sdrpp/p25_trunking_config.json` while SDR++ is closed.

## Testing without traffic

- `tests/test_retune_policy.cpp`: unit tests for the tuning policy
  (`g++ -std=c++17 -I src tests/test_retune_policy.cpp && ./a.out`).
- `tests/fake_dsdneo_client.py FREQ...`: acts like the decoder on the rtl_tcp link (with Launch
  dsd-neo off). It checks the header, the delivered sample rate, and tuning accuracy against
  steady carriers such as TV pilots.
- `tests/fake_api_server.py` and `tests/udp_audio_tx.py`: drive the panel with fake telemetry,
  and the player with test tones.

## Legal

This is receive-only software. It does not decrypt encrypted traffic, and must not be used to
try. Laws on monitoring and on repeating what you hear vary; know yours. IMBE/AMBE+2 vocoder
implementations may be patent-encumbered in some places (see NOTICE).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Changes are listed in
[CHANGELOG.md](CHANGELOG.md).
