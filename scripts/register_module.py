#!/usr/bin/env python3
"""Register the built module with SDR++ (run with SDR++ closed).

    python3 scripts/register_module.py /path/to/p25_trunking.so [--config-dir ~/.config/sdrpp]

Adds the .so to config.json "modules" and creates a "P25 Trunking" module instance.
config.json is backed up next to itself first.
"""

import argparse
import json
import shutil
import subprocess
import time
from pathlib import Path

INSTANCE = "P25 Trunking"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module", type=Path)
    ap.add_argument("--config-dir", type=Path, default=Path.home() / ".config/sdrpp")
    args = ap.parse_args()
    if subprocess.run(["pgrep", "-x", "sdrpp"], capture_output=True).returncode == 0:
        raise SystemExit("SDR++ is running; close it first (it rewrites config.json on exit).")
    so = args.module.expanduser().resolve()
    if not so.is_file():
        raise SystemExit(f"{so} not found; build and install the module first.")
    cfg_path = args.config_dir.expanduser() / "config.json"
    cfg = json.loads(cfg_path.read_text())
    shutil.copy(cfg_path, cfg_path.with_suffix(f".json.bak-{int(time.time())}"))
    mods = [m for m in cfg.get("modules", []) if Path(m).name != so.name]
    cfg["modules"] = mods + [str(so)]
    cfg.setdefault("moduleInstances", {})[INSTANCE] = {"module": "p25_trunking", "enabled": True}
    cfg_path.write_text(json.dumps(cfg, indent=4))
    print(f"Registered {so} as '{INSTANCE}' in {cfg_path}")


if __name__ == "__main__":
    main()
