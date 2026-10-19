# sdrpp-p25

An [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) module for **P25 trunking and digital
voice**: it follows a P25 Phase 1 control channel, tunes to Phase 1 (FDMA) and Phase 2 (TDMA)
voice grants, and plays the decoded voice through SDR++'s own audio output. Simulcast (CQPSK)
sites are supported. Encrypted calls are detected and skipped. It is an out-of-tree plugin:
no SDR++ fork is involved.

The decoding is done by [dsd-neo](https://github.com/arancormonk/dsd-neo), which the module runs
as a helper process. The module gives dsd-neo a virtual tuner backed by an SDR++ VFO, so you
keep SDR++'s waterfall, other VFOs and other modules while it trunks.

```
SDR++ ── RTL/Airspy/... source ──┬── your Radio VFO, other modules (unchanged)
                                 └── "P25 Trunking" VFO ── rtl_tcp server (127.0.0.1:14423)
                                                               │  IQ ▲ retune requests
                                                           dsd-neo (trunking, demod, vocoder)
          SDR++ audio sink ◄── paced player ◄── UDP voice ─────┘
          module panel     ◄── JSON-Lines API (calls, system, metrics)
```

## Features

- **P25 Phase 1 control channel, Phase 1 and Phase 2 voice**, with talkgroup and radio
  aliases, emergency flags and talker aliases where the system sends them.
- **Simulcast (CQPSK/LSM) and C4FM sites.**
- **Single-tuner trunking.** If a site's channels fit inside the SDR's bandwidth, the module
  centres on the site once and only moves its own VFO afterwards. Wider sites retune the SDR
  for each call and return to the control channel afterwards.
- **Encrypted calls skipped**, never decrypted, and shown as such in the call list.
- **Panel in SDR++:**
  - site profile picker, start and stop;
  - system and site identity, NAC, control and voice frequencies, SNR;
  - live slot 1 and slot 2 calls, with hold, lock out and skip;
  - recent calls, with per-talkgroup mute;
  - volume and slot routing;
  - decoder log.
- **SDRTrunk playlist import:** site frequencies and modulation become profiles, and alias
  lists become talkgroup lists. "Do not monitor" talkgroups are locked out.
- **Resumes automatically** when SDR++ starts, if it was running when SDR++ closed.

## Requirements

- Linux, and SDR++ 1.3 (nightly) installed from the official `.deb` builds. The distribution
  `sdrpp` packages are not compatible with out-of-tree modules.
- A receiver SDR++ can use (developed on an RTL-SDR at 2.4 MS/s).
- dsd-neo 2.11 or later. The portable AppImage from its releases page works and includes
  mbelib-neo.
- Build tools: `cmake`, a C++17 compiler, and `libvolk-dev`, `libfftw3-dev`, `libglfw3-dev` and
  `libzstd-dev` (SDR++'s own build dependencies).

## Build and install

SDR++ has no stable module ABI, so the module must be compiled against the core source of the
**exact commit** your SDR++ was built from. `SDRPP_COMMIT` names it, and the fetch script checks
it out.

```sh
git clone https://github.com/entitledosprey/sdrpp-p25.git && cd sdrpp-p25
scripts/fetch_sdrpp.sh                        # SDR++ source at SDRPP_COMMIT → third_party/
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake --install build --prefix ~/.local      # → ~/.local/lib/sdrpp/plugins/p25_trunking.so
# With SDR++ closed:
python3 scripts/register_module.py ~/.local/lib/sdrpp/plugins/p25_trunking.so
```

After upgrading SDR++, update `SDRPP_COMMIT` to the commit the new build came from, then rebuild.
Finding that commit: the nightly package version `1.2.1-<run>` is the run number of SDR++'s
"Build Binaries" workflow.

### dsd-neo

Download the `dsd-neo-linux-x86_64-portable-*.AppImage` from
[the releases](https://github.com/arancormonk/dsd-neo/releases) and make it executable. Then
either put it on `PATH` as `dsd-neo`, or enter its path in the module's Settings. The module
also looks in `~/.local/bin`, `/usr/local/bin` and `/usr/bin`.

AppImages need FUSE. To avoid that, extract it once with
`./dsd-neo-*.AppImage --appimage-extract` and point the module at a small wrapper script:

```sh
#!/bin/sh
exec /path/to/squashfs-root/AppRun "$@"
```

## Setting up a site

Profiles are JSON files in `~/.config/sdrpp/p25_trunking/profiles/`, one per site. The easiest
way to create them is from an SDRTrunk playlist:

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

- `modulation` is `CQPSK` for simulcast sites and `C4FM` otherwise.
- `control_channels` is the hunt list. Listing every site frequency is fine; the decoder finds
  the active control channel itself.
- `voice_frequencies` is filled in as calls are heard.
- `group_csv` is a [dsd-neo group list](https://github.com/arancormonk/dsd-neo/blob/main/docs/csv-formats.md)
  (`id,mode,name,tag`; mode `A` follows, `B` locks out).

## Using it

1. In SDR++, open the **P25 Trunking** panel, pick a profile and press **Start**.
2. The first time, check SDR++'s **Sinks** menu. The **P25 Trunking** stream should go to your
   audio device. Mute the **Radio** stream if you don't want its audio at the same time.
3. Calls appear in the panel as they're granted:
   - **Hold** stays on a talkgroup;
   - **Lock out** and **Skip** apply to the call in a slot;
   - **Mute TG** in the recent-calls list stops following that talkgroup.

While running, the module owns the SDR's tuning. If something else retunes SDR++ while the
decoder is connected, the module moves it back. Use a second receiver for other listening.

Untick **Launch dsd-neo** in Settings to run dsd-neo yourself: the panel shows the exact command,
which is handy for debugging.

## Notes and limits

- **One tuner:** on a site wider than the SDR bandwidth, the control channel is not monitored
  during a call. Calls on other talkgroups are missed until it returns. This is inherent to
  single-tuner trunking.
- **Simulcast decoding** depends on reception: a good antenna and gain matter more than
  anything else. The panel shows SNR, and the decoder log shows sync and errors.
- **Audio** starts about 0.25 s into each call. That buffer absorbs the decoder's bursty Phase 2
  output.
- **Ports used on 127.0.0.1:** 14423 (rtl_tcp), 9911 (decoder API), 23456 (voice). They can be
  changed in `~/.config/sdrpp/p25_trunking_config.json`.

## Testing without traffic

- `tests/test_retune_policy.cpp`: unit tests for the tuning policy
  (`g++ -std=c++17 -I src tests/test_retune_policy.cpp && ./a.out`).
- `tests/fake_dsdneo_client.py FREQ...`: acts like the decoder on the rtl_tcp link (Launch
  dsd-neo off). It checks the header, the sample rate and the tuning accuracy against steady
  carriers.
- `tests/fake_api_server.py` and `tests/udp_audio_tx.py`: feed the panel fake telemetry, and
  feed the player test tones.

## Legal

This is receive-only software. It does not decrypt encrypted traffic, and must not be used to
try. Laws on monitoring and on repeating what you hear vary; know yours. IMBE/AMBE+2 vocoder
implementations may be patent-encumbered in some places (see NOTICE).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Changes are listed in
[CHANGELOG.md](CHANGELOG.md).
