#!/bin/bash

set -euo pipefail

if [[ "$#" -lt 2 || "$#" -gt 3 ]]; then
    echo "usage: $0 SOURCE_BUNDLE DESTINATION_DIR [ARCHIVE_STEM]" >&2
    exit 2
fi

SOURCE_BUNDLE="$1"
DESTINATION_DIR="$2"
ARCHIVE_STEM="${3:-OuterShell.bundle}"
AA_TOOL="${AA_TOOL:-aa}"
ARCHIVE_BUNDLE_NAME="$ARCHIVE_STEM"

if [[ ! -d "$SOURCE_BUNDLE" ]]; then
    echo "error: source bundle not found at $SOURCE_BUNDLE" >&2
    exit 1
fi

if ! command -v "$AA_TOOL" >/dev/null 2>&1; then
    echo "error: required tool '$AA_TOOL' was not found on PATH" >&2
    exit 1
fi

if ! command -v lipo >/dev/null 2>&1; then
    echo "error: required tool 'lipo' was not found on PATH" >&2
    exit 1
fi

CODESIGN_IDENTITY="${OUTER_SHELL_CODESIGN_IDENTITY:--}"
NOTARY_PROFILE="${OUTER_SHELL_NOTARY_PROFILE:-}"
WORK_ROOT="$(mktemp -d)"
trap 'rm -rf "$WORK_ROOT" "$WORK_ROOT.zip"' EXIT
PREPARED_PLATFORMS=()
PREPARED_ROOTS=()
PREPARED_BUNDLES=()
PREPARED_ARCHIVES=()

macos_codesign_args() {
    if [[ "$CODESIGN_IDENTITY" == "-" ]]; then
        printf '%s\n' --force --sign - --timestamp=none
    else
        printf '%s\n' --force --options runtime --timestamp --sign "$CODESIGN_IDENTITY"
    fi
}

bundle_executable_name() {
    local bundle_path="$1"
    local info_plist="$bundle_path/Contents/Info.plist"
    local executable_name=""

    if [[ -f "$info_plist" && -x /usr/libexec/PlistBuddy ]]; then
        executable_name="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$info_plist" 2>/dev/null || true)"
    fi

    if [[ -z "$executable_name" ]]; then
        executable_name="$(basename "$bundle_path" .bundle)"
    fi

    printf '%s\n' "$executable_name"
}

archive_platform_bundle() {
    local source_bundle="$1"
    local destination_dir="$2"
    local platform="$3"
    local slice_arch="$4"
    local archive_path="$5"
    local archive_bundle_name="$6"
    local temp_root
    local temp_bundle
    local executable_name
    local executable_path
    local archived_executable_name
    local archived_executable_path
    local thinned_executable
    local executable_archs

    temp_root="$WORK_ROOT/$platform"
    mkdir -p "$temp_root"
    temp_bundle="$temp_root/$archive_bundle_name"
    cp -R "$source_bundle" "$temp_bundle"

    executable_name="$(bundle_executable_name "$temp_bundle")"
    executable_path="$temp_bundle/Contents/MacOS/$executable_name"
    if [[ ! -f "$executable_path" ]]; then
        echo "error: expected bundle executable at $executable_path" >&2
        rm -rf "$temp_root"
        exit 1
    fi

    executable_archs="$(lipo -archs "$executable_path")"
    case " $executable_archs " in
        *" $slice_arch "*) ;;
        *)
            echo "error: $executable_path does not contain $slice_arch (found: $executable_archs)" >&2
            rm -rf "$temp_root"
            exit 1
            ;;
    esac

    if [[ "$executable_archs" != "$slice_arch" ]]; then
        thinned_executable="$temp_root/$executable_name.$slice_arch"
        lipo "$executable_path" -thin "$slice_arch" -output "$thinned_executable"
        mv "$thinned_executable" "$executable_path"
        chmod +x "$executable_path"
    fi

    archived_executable_name="${archive_bundle_name%.bundle}"
    archived_executable_path="$temp_bundle/Contents/MacOS/$archived_executable_name"
    if [[ "$archived_executable_name" != "$executable_name" ]]; then
        mv "$executable_path" "$archived_executable_path"
        chmod +x "$archived_executable_path"
        if [[ -x /usr/libexec/PlistBuddy ]]; then
            /usr/libexec/PlistBuddy -c "Set :CFBundleExecutable $archived_executable_name" "$temp_bundle/Contents/Info.plist"
        fi
    fi

    if command -v /usr/bin/codesign >/dev/null 2>&1; then
        local codesign_args=()
        while IFS= read -r arg; do
            codesign_args+=("$arg")
        done < <(macos_codesign_args)
        /usr/bin/codesign "${codesign_args[@]}" "$temp_bundle" >/dev/null
    fi

    PREPARED_PLATFORMS+=("$platform")
    PREPARED_ROOTS+=("$temp_root")
    PREPARED_BUNDLES+=("$temp_bundle")
    PREPARED_ARCHIVES+=("$archive_path")
}

notarize_prepared_bundles_if_requested() {
    if [[ -z "$NOTARY_PROFILE" ]]; then
        return 0
    fi
    if [[ "$CODESIGN_IDENTITY" == "-" ]]; then
        echo "error: a notary profile requires a Developer ID codesign identity" >&2
        exit 1
    fi
    /usr/bin/ditto -c -k --norsrc "$WORK_ROOT" "$WORK_ROOT.zip"
    xcrun notarytool submit "$WORK_ROOT.zip" \
        --keychain-profile "$NOTARY_PROFILE" \
        --wait
    local bundle
    for bundle in "${PREPARED_BUNDLES[@]}"; do
        xcrun stapler staple "$bundle"
    done
}

archive_prepared_bundles() {
    local index
    for index in "${!PREPARED_BUNDLES[@]}"; do
        "$AA_TOOL" archive \
            -d "${PREPARED_ROOTS[$index]}" \
            -subdir "$(basename "${PREPARED_BUNDLES[$index]}")" \
            -o "${PREPARED_ARCHIVES[$index]}" \
            -a lzfse
        echo "Archived ${PREPARED_PLATFORMS[$index]} bundle at ${PREPARED_ARCHIVES[$index]}"
    done
}

mkdir -p "$DESTINATION_DIR"
SYMBOL_BUNDLE="${SOURCE_BUNDLE}.dSYM"
if [[ -d "$SYMBOL_BUNDLE" ]]; then
    while read -r symbol_uuid; do
        symbol_directory="$DESTINATION_DIR/symbols/$symbol_uuid"
        mkdir -p "$symbol_directory"
        ditto "$SYMBOL_BUNDLE" "$symbol_directory/$(basename "$SYMBOL_BUNDLE")"
    done < <(dwarfdump --uuid "$SYMBOL_BUNDLE" | awk '{print $2}')
else
    echo "error: missing debug symbols at $SYMBOL_BUNDLE" >&2
    exit 1
fi
archive_platform_bundle "$SOURCE_BUNDLE" "$DESTINATION_DIR" "macos-arm" "arm64" "$DESTINATION_DIR/$ARCHIVE_STEM.macos-arm.aar" "$ARCHIVE_BUNDLE_NAME"
archive_platform_bundle "$SOURCE_BUNDLE" "$DESTINATION_DIR" "macos-x86" "x86_64" "$DESTINATION_DIR/$ARCHIVE_STEM.macos-x86.aar" "$ARCHIVE_BUNDLE_NAME"
notarize_prepared_bundles_if_requested
archive_prepared_bundles
