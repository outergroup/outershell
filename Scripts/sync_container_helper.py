#!/usr/bin/env python3
"""Copy Resources/ContainerHelper.h into the apps that run container helpers."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = root / "Backends" / "Resources" / "ContainerHelper.h"
for target in [root / "Files" / "Backend" / "Environment" / "ContainerHelper.h",
               root / "Plaintext" / "Backend" / "Environment" / "ContainerHelper.h",
               root / "Top" / "Backend" / "Environment" / "ContainerHelper.h",
               root / "Trace" / "Backend" / "ContainerHelper.h",
               root / "Profile" / "Backend" / "ContainerHelper.h"]:
    if args.check:
        if not target.exists() or target.read_bytes() != source.read_bytes():
            raise SystemExit(f"ContainerHelper.h differs from {source}: {target}")
    else:
        target.write_bytes(source.read_bytes())
print("All copies of ContainerHelper.h match.")
