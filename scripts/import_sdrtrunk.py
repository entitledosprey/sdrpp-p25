#!/usr/bin/env python3
"""Import an SDRTrunk playlist into P25 Trunking profiles.

    python3 scripts/import_sdrtrunk.py PLAYLIST.xml [--out ~/.config/sdrpp/p25_trunking]

For every P25 <channel> (one site) it writes profiles/<system>_<site>.json with the site's
frequencies and modulation, and for every alias list it writes groups/<list>.csv in
dsd-neo's group format. Talkgroups with SDRTrunk priority -1 ("do not monitor") become
mode B (locked out). SDRTrunk playlists only hold control-channel candidates; the module
learns voice frequencies as calls are heard.
"""

import argparse
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path


def slug(text):
    return re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_") or "unnamed"


def clean(text):
    return (text or "").replace(",", ";").replace("\n", " ").strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("playlist", type=Path)
    ap.add_argument("--out", type=Path, default=Path.home() / ".config/sdrpp/p25_trunking")
    args = ap.parse_args()
    root = ET.parse(args.playlist.expanduser()).getroot()
    out = args.out.expanduser()
    (out / "profiles").mkdir(parents=True, exist_ok=True)
    (out / "groups").mkdir(parents=True, exist_ok=True)

    groups = {}
    for alias in root.iter("alias"):
        lst = alias.get("list") or "default"
        locked = any(i.get("type") == "priority" and i.get("priority") == "-1" for i in alias.iter("id"))
        for i in alias.iter("id"):
            if i.get("protocol") not in (None, "APCO25"):
                continue
            if i.get("type") == "talkgroup" and i.get("value"):
                key = i.get("value")
            elif i.get("type") == "talkgroupRange" and i.get("min") and i.get("max"):
                key = f"{i.get('min')}-{i.get('max')}"
            else:
                continue
            groups.setdefault(lst, []).append((key, "B" if locked else "A", clean(alias.get("name")),
                                               clean(alias.get("group"))))
    for lst, rows in groups.items():
        path = out / "groups" / f"{slug(lst)}.csv"
        path.write_text("id,mode,name,tag\n" + "".join(f"{r[0]},{r[1]},{r[2]},{r[3]}\n" for r in rows))
        print(f"groups/{path.name}: {len(rows)} talkgroups ({sum(r[1] == 'B' for r in rows)} locked out)")

    for ch in root.iter("channel"):
        dec = ch.find("decode_configuration")
        if dec is None or "P25" not in (dec.get("type") or ""):
            continue
        freqs = sorted({int(f.text) for f in ch.iter("frequency") if f.text})
        if not freqs:
            continue
        system = ch.get("system") or "System"
        site = re.sub(r"\s*\(.*\)", "", ch.get("site") or ch.get("name") or "Site")
        lst = (ch.findtext("alias_list_name") or "").strip()
        profile = {
            "name": f"{system} - {site}",
            "system": system,
            "modulation": "CQPSK" if (dec.get("modulation") or "").upper() == "CQPSK" else "C4FM",
            "control_channels": freqs,
            "voice_frequencies": [],
            "group_csv": f"groups/{slug(lst)}.csv" if lst in groups else "",
        }
        path = out / "profiles" / f"{slug(system)}_{slug(site)}.json"
        if path.exists():  # keep learned voice frequencies across re-imports
            old = json.loads(path.read_text())
            profile["voice_frequencies"] = old.get("voice_frequencies", [])
        path.write_text(json.dumps(profile, indent=2))
        span = (max(freqs) - min(freqs)) / 1e6
        print(f"profiles/{path.name}: {profile['modulation']}, {len(freqs)} frequencies, span {span:.3f} MHz")


if __name__ == "__main__":
    main()
