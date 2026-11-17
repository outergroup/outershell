#!/usr/bin/env python3
"""Refresh the explicitly named apps' vendored outerframe Swift helpers.

Copies OuterframeHost and SocketToBrowser from this directory, and the protocol,
accessibility, and Common files from an outerframe checkout, which owns them.
"""
from pathlib import Path
import argparse
import shutil

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('apps', nargs='+', type=Path, help='App directories that contain Vendor or Frontend/Vendor')
parser.add_argument('--outerframe', type=Path, default=source.parents[2] / 'outerframe',
                    help='outerframe checkout (default: next to this outershell checkout)')
args = parser.parse_args()
common = args.outerframe / 'macOS' / 'Common'
if not common.is_dir():
    parser.error(f'{args.outerframe} is not an outerframe checkout; pass --outerframe')

methods_files = {
    'OuterframeHost.swift': source / 'OuterframeHost.swift',
    'SocketToBrowser.swift': source / 'SocketToBrowser.swift',
    'OuterContentSocketMessage.swift': common / 'OuterframeContentSocketMessage.swift',
    'OuterframeAccessibility.swift': common / 'OuterframeAccessibility.swift',
}
common_files = ['OuterframeAppConnection.swift', 'OuterframeContentController.swift', 'SafeDispatchSource.swift']

for app in args.apps:
    vendor = app / 'Vendor'
    if not vendor.is_dir():
        vendor = app / 'Frontend' / 'Vendor'
    if not vendor.is_dir():
        parser.error(f'{app} has no Vendor directory')
    methods = vendor / 'OuterframeSwiftMethods'
    methods.mkdir(exist_ok=True)
    for name, path in methods_files.items():
        shutil.copyfile(path, methods / name)
    (vendor / 'Common').mkdir(exist_ok=True)
    for name in common_files:
        shutil.copyfile(common / name, vendor / 'Common' / name)
