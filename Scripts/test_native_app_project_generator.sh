#!/bin/bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "${TEST_ROOT}"' EXIT

if [[ "$(uname -s)" == Darwin ]]; then
    xcrun swiftc \
        "${REPO_ROOT}/Frontend/NativeAppProjectGenerator.swift" \
        "${REPO_ROOT}/Scripts/NativeAppIconGeneratorTest.swift" \
        -o "${TEST_ROOT}/native-app-icon-generator-test"
    "${TEST_ROOT}/native-app-icon-generator-test"
fi

PROJECTS_ROOT="${TEST_ROOT}/projects"
RESPONSE_ROOT="${TEST_ROOT}/response"
PROJECT_ROOT="${PROJECTS_ROOT}/widget-works"
BUILDER_ROOT="${RESPONSE_ROOT}/builder/widget-works-macOS"
ICON_SOURCE="${REPO_ROOT}/app-icon.png"

bash -n "${REPO_ROOT}/Resources/NativeAppTemplate/app"

python3 "${REPO_ROOT}/Resources/NativeAppTemplate/create-project.py" \
    --name "Widget Works" \
    --app-id "com.example.WidgetWorks" \
    --scheme "WidgetWorks" \
    --source-root "${PROJECTS_ROOT}" \
    --folder "widget-works" \
    --socket "com.example.WidgetWorksSocket" \
    --targets "html macos" \
    --macos-language swift \
    --backend-language go \
    --isolation host \
    --icon "${ICON_SOURCE}" \
    --builder-output "${RESPONSE_ROOT}" \
    --skip-deploy

test -x "${PROJECT_ROOT}/app"
test -d "${PROJECT_ROOT}/html"
test -d "${PROJECT_ROOT}/macos/WidgetWorks.xcodeproj"
test -d "${PROJECT_ROOT}/server"
test ! -e "${PROJECT_ROOT}/macos-builder"
test ! -e "${PROJECT_ROOT}/frontend-swift"
test ! -e "${PROJECT_ROOT}/frontend-objc"
test ! -e "${PROJECT_ROOT}/backend-go"
test ! -e "${PROJECT_ROOT}/backend-c"
test ! -e "${PROJECT_ROOT}/Dockerfile"
test -f "${PROJECT_ROOT}/server/.dockerignore"
bash -n "${PROJECT_ROOT}/app"
grep -Fq 'backend_build_go_in_apple_container' "${PROJECT_ROOT}/app"
grep -Fq 'backend_build_go_in_docker' "${PROJECT_ROOT}/app"
grep -Fq 'container system status' "${PROJECT_ROOT}/app"
grep -Fq 'OUTER_BUILD_CONTAINER_RUNTIME' "${PROJECT_ROOT}/app"

cmp "${ICON_SOURCE}" "${PROJECT_ROOT}/app-icon.png"
cmp "${ICON_SOURCE}" "${PROJECT_ROOT}/macos/app-icon.png"
cmp "${ICON_SOURCE}" "${BUILDER_ROOT}/app-icon.png"

test "$(cat "${RESPONSE_ROOT}/.outershell-remote-project-path")" = "${PROJECT_ROOT}"
test -x "${BUILDER_ROOT}/platform"
test -f "${BUILDER_ROOT}/README.md"
test -d "${BUILDER_ROOT}/macos/WidgetWorks.xcodeproj"
bash -n "${BUILDER_ROOT}/platform"

grep -Fq 'REMOTE_PROJECT="'"${PROJECT_ROOT}"'"' "${BUILDER_ROOT}/platform"
grep -Fq "Widget Works" "${BUILDER_ROOT}/platform"
grep -Fq "${PROJECT_ROOT}" "${BUILDER_ROOT}/README.md"
if grep -R -E '__REMOTE_PROJECT|HelloFullstack|Hello World' "${PROJECT_ROOT}" "${BUILDER_ROOT}" >/dev/null; then
    echo "error: generated project still contains template placeholders" >&2
    exit 1
fi

HTML_RESPONSE_ROOT="${TEST_ROOT}/html-response"
python3 "${REPO_ROOT}/Resources/NativeAppTemplate/create-project.py" \
    --name "Web Only" \
    --app-id "com.example.WebOnly" \
    --scheme "WebOnly" \
    --source-root "${PROJECTS_ROOT}" \
    --folder "web-only" \
    --socket "com.example.WebOnlySocket" \
    --targets html \
    --macos-language swift \
    --backend-language c \
    --isolation container \
    --builder-output "${HTML_RESPONSE_ROOT}" \
    --skip-deploy

test -d "${PROJECTS_ROOT}/web-only/html"
test ! -e "${PROJECTS_ROOT}/web-only/macos"
test -f "${PROJECTS_ROOT}/web-only/Dockerfile"
test -x "${PROJECTS_ROOT}/web-only/app"
test ! -e "${HTML_RESPONSE_ROOT}/builder"
test "$(cat "${HTML_RESPONSE_ROOT}/.outershell-remote-project-path")" = "${PROJECTS_ROOT}/web-only"
bash -n "${PROJECTS_ROOT}/web-only/app"
grep -Fq 'container build --progress plain' "${PROJECTS_ROOT}/web-only/app"
grep -Fq 'docker build -t' "${PROJECTS_ROOT}/web-only/app"

echo "Native app project generator tests passed."
