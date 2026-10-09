#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
files="${1:-$root/../Files}"
out="$root/build/wsl"
mkdir -p "$out"
cc -std=gnu17 -O2 -o "$out/outershelld" "$root/Backend/OuterShellBuffer.c" "$root/Backend/OuterShellAPI.c" "$root/Backend/OuterShellImage.c" "$root/Backend/OuterShellPlatform.c" "$root/outershelld/OuterService.c" "$root/outershelld/outershelld.c" -lpng -lz -ldl -lpthread -lm
cc -std=gnu17 -O2 -DOUTER_SHELL_BACKEND_STANDALONE=1 -DOUTER_SHELL_USE_LIBCURL=1 -o "$out/OuterShellBackend" "$root/Backend/OuterShellBuffer.c" "$root/Backend/OuterShellAPI.c" "$root/Backend/OuterShellPlatform.c" "$root/Backend/OuterShellDownloader.c" "$root/Backend/OuterShellBackend.c" -lcurl -ldl -lpthread -lm
c++ -std=c++17 -O2 -o "$out/outerctl" "$root/Resources/outerctl.cpp"
cc -std=gnu17 -O2 -Wall -Wextra -o "$out/outer-socket-bridge" "$root/SocketBridge/outer-socket-bridge.c"
cc -std=gnu17 -O2 -o "$out/FilesBackend" "$files/Backend/main.c"
printf 'Built WSL support in %s\n' "$out"
