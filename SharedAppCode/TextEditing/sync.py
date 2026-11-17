#!/usr/bin/env python3
"""Refresh the explicitly named apps' vendored text history copies."""
from pathlib import Path
import argparse
import shutil

parser = argparse.ArgumentParser()
parser.add_argument('apps', nargs='+', type=Path, help='App directories that contain Vendor or Frontend/Vendor')
args = parser.parse_args()
source = Path(__file__).resolve().parent
for app in args.apps:
    vendor = app / 'Vendor'
    if not vendor.is_dir():
        vendor = app / 'Frontend' / 'Vendor'
    if not vendor.is_dir():
        parser.error(f'{app} has no Vendor directory')
    target = vendor / 'TextEditing'
    target.mkdir(exist_ok=True)
    shutil.copyfile(source / 'TextUndoHistory.swift', target / 'TextUndoHistory.swift')
    shutil.copyfile(source / 'Vendored-README.md', target / 'README.md')
