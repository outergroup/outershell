#!/usr/bin/env python3
"""Sync catalog and development web icons from sibling app repositories."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
for repo, name in [("Top", "Top"), ("Files", "Files"), ("Plaintext", "Plaintext"), ("Profile", "Profile"), ("Trace", "Firehose")]:
    source = root / repo / "app-icon.png"
    for target in [root / "Backends" / "Resources" / "OuterShellWeb" / "icons" / f"org.outershell.{name}.png",
                   root / repo / "Resources" / f"{name}Web" / "app-icon.png"]:
        if args.check:
            if not target.exists() or target.read_bytes() != source.read_bytes():
                raise SystemExit(f"Icon differs from {source}: {target}")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.read_bytes())
print("All bundled app icons match their source assets.")
