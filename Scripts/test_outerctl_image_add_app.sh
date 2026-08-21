#!/bin/bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_root="$(mktemp -d)"
trap 'rm -rf "${test_root}"' EXIT

c++ -std=c++17 -Wall -Wextra -Werror \
    -o "${test_root}/outerctl" \
    "${repo_root}/Resources/outerctl.cpp"

native_root="${test_root}/native"
OUTERCTL_IMAGE_ROOT="${native_root}" "${test_root}/outerctl" image add-app \
    --id jupyter-work \
    --name "JupyterLab: work" \
    --url /lab \
    --working-directory /work \
    --socket-argument=--ServerApp.sock \
    -- /opt/venvs/default/bin/jupyter-lab --no-browser

native_service="${native_root}/var/lib/outershell/services/org.outershell.image.jupyter-work.outerservice"
native_registration="${native_root}/etc/outershell/apps.d/org.outershell.image.jupyter-work.sh"
grep -Fx 'Executable=/opt/venvs/default/bin/jupyter-lab' "${native_service}"
grep -Fx 'Argument=--ServerApp.sock=/run/user/0/org.outershell.image.jupyter-work' "${native_service}"
grep -F -- "--url '/lab'" "${native_registration}"
test -d "${native_root}/work"
/bin/sh -n "${native_registration}"

tcp_root="${test_root}/tcp"
OUTERCTL_IMAGE_ROOT="${tcp_root}" "${test_root}/outerctl" image add-app \
    --id rstudio \
    --name "RStudio Server" \
    --tcp-port 8787 \
    -- /usr/lib/rstudio-server/bin/rserver --server-daemonize=0

tcp_service="${tcp_root}/var/lib/outershell/services/org.outershell.image.rstudio.outerservice"
tcp_wrapper="${tcp_root}/opt/outershell/image-apps/org.outershell.image.rstudio/start"
grep -Fx 'Executable=/opt/outershell/image-apps/org.outershell.image.rstudio/start' "${tcp_service}"
grep -F 'port=8787' "${tcp_wrapper}"
grep -F 'UNIX-LISTEN:"${socket}"' "${tcp_wrapper}"
/bin/bash -n "${tcp_wrapper}"

if OUTERCTL_IMAGE_ROOT="${test_root}/invalid" "${test_root}/outerctl" image add-app \
    --id invalid --name Invalid --socket-argument=--socket --tcp-port 8000 \
    -- /bin/true >/dev/null 2>&1; then
    echo "error: mutually exclusive endpoint options were accepted" >&2
    exit 1
fi

command_root="${test_root}/commands"
mkdir -p "${command_root}/icons"
cp "${repo_root}/app-icon.png" "${command_root}/icons/codex.png"
OUTERCTL_IMAGE_ROOT="${command_root}" "${test_root}/outerctl" image add-command \
    --id codex \
    --name "Codex CLI" \
    --working-directory /work \
    --user scientist \
    --icon-path /icons/codex.png \
    -- /usr/local/bin/codex --search

command_manifest="${command_root}/etc/outershell/commands.d/codex.outercommand"
grep -Fx 'Name=Codex CLI' "${command_manifest}"
grep -Fx 'IconPath=/icons/codex.png' "${command_manifest}"
grep -Fx 'Executable=/usr/local/bin/codex' "${command_manifest}"
grep -Fx 'Argument=--search' "${command_manifest}"
test -d "${command_root}/work"

command_list="$(OUTERCTL_IMAGE_ROOT="${command_root}" "${test_root}/outerctl" image list-commands)"
case "${command_list}" in
    $'1\tcodex\tCodex CLI\t/work\tscientist\t/icons/codex.png\t/usr/local/bin/codex\t--search') ;;
    *)
        echo "error: unexpected command list: ${command_list}" >&2
        exit 1
        ;;
esac

if OUTERCTL_IMAGE_ROOT="${command_root}" "${test_root}/outerctl" image add-command \
    --id missing-icon \
    --name "Missing Icon" \
    --icon-path /icons/missing.png \
    -- /bin/sh >/dev/null 2>&1; then
    echo "error: missing command icon was accepted" >&2
    exit 1
fi

echo "outerctl image registration tests passed"
