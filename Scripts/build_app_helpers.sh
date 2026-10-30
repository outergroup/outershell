#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for app in Files Plaintext Top Profile Trace; do
    name="outer-$(echo "$app" | tr '[:upper:]' '[:lower:]')-helper"
    if [[ "$app" == Trace ]]; then name=outer-firehose-helper; fi
    for architecture in aarch64 x86_64; do
        "${root}/../${app}/Helper/build.sh" linux "${architecture}" \
            "${root}/build/linux-package/RemoteLinuxBinariesMusl/${architecture}/${name}"
    done
done
