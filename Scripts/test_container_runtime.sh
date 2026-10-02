#!/bin/sh
set -eu
repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT INT TERM
xcrun swiftc -module-cache-path "$temporary/modules" \
    "$repo_root/Agent/ContainerRuntime.swift" "$repo_root/Scripts/test_container_runtime.swift" \
    -o "$temporary/test"
"$temporary/test"
