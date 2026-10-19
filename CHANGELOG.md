# Changelog

## 0.1.0 (2026-10-19)

First working version.

- SDR++ module (out of tree, built against the SDR++ core source at a pinned commit) that runs
  dsd-neo as a helper process. The module provides:
  - a virtual rtl_tcp tuner backed by its own SDR++ VFO;
  - a tuning policy that keeps sites that fit inside the SDR bandwidth on one centre;
  - decoded voice played through SDR++'s audio sinks;
  - a panel fed by the decoder's JSON-Lines API.
- Verified on a live P25 Phase II simulcast site:
  - control channel decoded at 100% FEC;
  - Phase 2 TDMA voice followed with talkgroup and talker aliases;
  - 0 voice errors at ~20 dB SNR;
  - encrypted grants skipped.
- SDRTrunk playlist import (`scripts/import_sdrtrunk.py`), module registration script, test
  tools (retune unit test, fake decoder client, fake API server, UDP tone sender).
- Audio player paced by the audio sink's own clock, with a 250 ms per-call prebuffer.
  (A timer-paced player was choppy because it made the sink's callback wait.)
- Decoder lifecycle: separate process group, killed with SDR++, restarted with backoff after
  crashes, resumes when SDR++ starts if it was running at exit.
