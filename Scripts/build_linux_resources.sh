#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RESOURCES_DIR="${REPO_ROOT}/Resources"
SQLITE_DIR="${RESOURCES_DIR}/ThirdParty/sqlite"

case "$(uname -m)" in
    aarch64|arm64)
        ARCH="aarch64"
        ;;
    x86_64|amd64)
        ARCH="x86_64"
        ;;
    *)
        echo "error: unsupported Linux architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing $1" >&2
        exit 1
    fi
}

require_file "${SQLITE_DIR}/sqlite3.c"
require_file "${SQLITE_DIR}/sqlite3.h"
require_file "${RESOURCES_DIR}/outerctl.cpp"

install_linux_static_build_deps() {
    if [[ -f /usr/lib64/libz.a ]]; then
        return
    fi
    if ! command -v yum >/dev/null 2>&1; then
        echo "error: static zlib is missing and yum is unavailable" >&2
        exit 1
    fi
    if [[ "${EUID}" -ne 0 ]]; then
        echo "error: static zlib is missing; rerun in the Docker build image or install it first" >&2
        exit 1
    fi
    yum -y install pkgconfig perl-IPC-Cmd perl-Time-Piece zlib-devel zlib-static
}

download_build_file() {
    local url="$1"
    local out="$2"
    local python_bin
    python_bin="$(command -v python3 || command -v python || true)"
    if [[ -z "$python_bin" ]]; then
        echo "error: python is required to download build dependencies" >&2
        exit 1
    fi
    "$python_bin" - "$url" "$out" <<'PY'
import sys
try:
    from urllib.request import urlopen
except ImportError:
    from urllib2 import urlopen

url, out = sys.argv[1], sys.argv[2]
response = urlopen(url, timeout=60)
try:
    data = response.read()
finally:
    response.close()
with open(out, "wb") as file:
    file.write(data)
PY
}

build_static_openssl() {
    local openssl_version="${OUTER_SHELL_OPENSSL_VERSION:-3.5.6}"
    local openssl_url="${OUTER_SHELL_OPENSSL_SOURCE_URL:-https://www.openssl.org/source/openssl-${openssl_version}.tar.gz}"
    local deps_root="${REPO_ROOT}/build/linux-deps/${ARCH}"
    local openssl_prefix="${deps_root}/openssl-${openssl_version}-install"
    local openssl_archive="${deps_root}/openssl-${openssl_version}.tar.gz"
    local openssl_source="${deps_root}/openssl-${openssl_version}"
    local openssl_build_log="${deps_root}/openssl-${openssl_version}.build.log"
    local openssl_build_stamp="${openssl_prefix}/.outer-shell-build-flags"
    local openssl_build_flags="v2 no-shared no-tests no-docs no-apps no-dso no-module Os function-sections data-sections"
    local openssl_target

    if [[ -f "${openssl_prefix}/include/openssl/ssl.h" ]] &&
       find "${openssl_prefix}" -name libssl.a -print -quit | grep -q . &&
       find "${openssl_prefix}" -name libcrypto.a -print -quit | grep -q . &&
       [[ -f "${openssl_build_stamp}" ]] &&
       [[ "$(cat "${openssl_build_stamp}")" == "${openssl_build_flags}" ]]; then
        STATIC_OPENSSL_PREFIX="${openssl_prefix}"
        return
    fi

    install_linux_static_build_deps
    mkdir -p "${deps_root}"
    if [[ ! -f "${openssl_archive}" ]]; then
        echo "==> Downloading OpenSSL ${openssl_version}"
        download_build_file "${openssl_url}" "${openssl_archive}"
    fi
    rm -rf "${openssl_source}" "${openssl_prefix}"
    tar -xzf "${openssl_archive}" -C "${deps_root}"

    case "${ARCH}" in
        x86_64)
            openssl_target="linux-x86_64"
            ;;
        aarch64)
            openssl_target="linux-aarch64"
            ;;
        *)
            echo "error: unsupported OpenSSL target architecture: ${ARCH}" >&2
            exit 1
            ;;
    esac

    echo "==> Building static OpenSSL ${openssl_version} for ${ARCH}"
    if ! (
        cd "${openssl_source}"
        ./Configure "${openssl_target}" \
            --prefix="${openssl_prefix}" \
            --openssldir="${openssl_prefix}/ssl" \
            -Os \
            -ffunction-sections \
            -fdata-sections \
            no-shared \
            no-tests \
            no-docs \
            no-apps \
            no-dso \
            no-module
        make -j"$(nproc 2>/dev/null || echo 2)" build_libs
        make install_dev install_runtime_libs
    ) >"${openssl_build_log}" 2>&1; then
        echo "error: failed to build OpenSSL ${openssl_version}; showing tail of ${openssl_build_log}" >&2
        tail -200 "${openssl_build_log}" >&2 || true
        exit 1
    fi
    printf '%s\n' "${openssl_build_flags}" > "${openssl_build_stamp}"
    STATIC_OPENSSL_PREFIX="${openssl_prefix}"
}

static_openssl_library() {
    local prefix="$1"
    local name="$2"
    local path
    path="$(find "${prefix}" -name "${name}" -print -quit)"
    if [[ -z "${path}" ]]; then
        echo "error: missing ${name} under ${prefix}" >&2
        exit 1
    fi
    printf '%s\n' "${path}"
}

build_static_libcurl() {
    local curl_version="${OUTER_SHELL_CURL_VERSION:-8.20.0}"
    local curl_url="${OUTER_SHELL_CURL_SOURCE_URL:-https://curl.se/download/curl-${curl_version}.tar.gz}"
    local deps_root="${REPO_ROOT}/build/linux-deps/${ARCH}"
    local curl_prefix="${deps_root}/curl-${curl_version}-install"
    local curl_archive="${deps_root}/curl-${curl_version}.tar.gz"
    local curl_source="${deps_root}/curl-${curl_version}"
    local curl_build_log="${deps_root}/curl-${curl_version}.build.log"
    local curl_build_stamp="${curl_prefix}/.outer-shell-build-flags"
    local curl_build_flags="v2 minimal-http-https Os function-sections data-sections gc-sections"

    if [[ -f "${curl_prefix}/lib/pkgconfig/libcurl.pc" ]] &&
       [[ -f "${curl_prefix}/lib/libcurl.a" ]] &&
       [[ -f "${curl_build_stamp}" ]] &&
       [[ "$(cat "${curl_build_stamp}")" == "${curl_build_flags}" ]]; then
        STATIC_LIBCURL_PREFIX="${curl_prefix}"
        return
    fi

    install_linux_static_build_deps
    mkdir -p "${deps_root}"
    if [[ ! -f "${curl_archive}" ]]; then
        echo "==> Downloading curl ${curl_version}"
        download_build_file "${curl_url}" "${curl_archive}"
    fi
    rm -rf "${curl_source}" "${curl_prefix}"
    tar -xzf "${curl_archive}" -C "${deps_root}"

    echo "==> Building static curl ${curl_version} for ${ARCH}"
    if ! (
        cd "${curl_source}"
        openssl_pc_path="${STATIC_OPENSSL_PREFIX}/lib64/pkgconfig:${STATIC_OPENSSL_PREFIX}/lib/pkgconfig"
        export PKG_CONFIG_PATH="${openssl_pc_path}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
        export CFLAGS="-Os -ffunction-sections -fdata-sections"
        export LDFLAGS="-Wl,--gc-sections"
        ./configure \
            --prefix="${curl_prefix}" \
            --disable-shared \
            --enable-static \
            --with-openssl="${STATIC_OPENSSL_PREFIX}" \
            --without-libpsl \
            --without-libidn2 \
            --without-nghttp2 \
            --without-brotli \
            --without-zstd \
            --disable-alt-svc \
            --disable-ftp \
            --disable-file \
            --disable-ipfs \
            --disable-ldap \
            --disable-ldaps \
            --disable-rtsp \
            --disable-dict \
            --disable-telnet \
            --disable-tftp \
            --disable-pop3 \
            --disable-imap \
            --disable-smb \
            --disable-smtp \
            --disable-gopher \
            --disable-mqtt \
            --disable-websockets \
            --disable-cookies \
            --disable-mime \
            --disable-form-api \
            --disable-netrc \
            --disable-http-auth \
            --disable-aws \
            --disable-tls-srp \
            --disable-hsts \
            --disable-headers-api \
            --disable-libcurl-option \
            --disable-threaded-resolver \
            --disable-verbose \
            --disable-progress-meter \
            --disable-get-easy-options \
            --disable-dateparse \
            --disable-manual
        make -j"$(nproc 2>/dev/null || echo 2)"
        make install
    ) >"${curl_build_log}" 2>&1; then
        echo "error: failed to build curl ${curl_version}; showing tail of ${curl_build_log}" >&2
        tail -200 "${curl_build_log}" >&2 || true
        exit 1
    fi
    printf '%s\n' "${curl_build_flags}" > "${curl_build_stamp}"
    STATIC_LIBCURL_PREFIX="${curl_prefix}"
}

install_linux_static_build_deps
STATIC_OPENSSL_PREFIX=""
build_static_openssl
OPENSSL_SSL_STATIC_LIB="$(static_openssl_library "${STATIC_OPENSSL_PREFIX}" libssl.a)"
OPENSSL_CRYPTO_STATIC_LIB="$(static_openssl_library "${STATIC_OPENSSL_PREFIX}" libcrypto.a)"
STATIC_LIBCURL_PREFIX=""
build_static_libcurl
CURL_PREFIX="${STATIC_LIBCURL_PREFIX}"
export PKG_CONFIG_PATH="${CURL_PREFIX}/lib/pkgconfig:${STATIC_OPENSSL_PREFIX}/lib64/pkgconfig:${STATIC_OPENSSL_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
read -r -a LIBCURL_CFLAGS <<< "$(pkg-config --cflags libcurl)"
LIBCURL_STATIC_LIB="${CURL_PREFIX}/lib/libcurl.a"
if [[ ! -f "${LIBCURL_STATIC_LIB}" ]]; then
    echo "error: missing static libcurl at ${LIBCURL_STATIC_LIB}" >&2
    exit 1
fi
read -r -a LIBCURL_LIBS_RAW <<< "$(pkg-config --static --libs libcurl)"
LIBCURL_LIBS=()
for flag in "${LIBCURL_LIBS_RAW[@]}"; do
    case "$flag" in
        -lcurl)
            ;;
        -lssl)
            LIBCURL_LIBS+=("${OPENSSL_SSL_STATIC_LIB}")
            ;;
        -lcrypto)
            LIBCURL_LIBS+=("${OPENSSL_CRYPTO_STATIC_LIB}")
            ;;
        -lz)
            LIBCURL_LIBS+=("/usr/lib64/libz.a")
            ;;
        *)
            LIBCURL_LIBS+=("${flag}")
            ;;
    esac
done

OUTPUT_DIR="${REPO_ROOT}/build/linux-package/RemoteLinuxBinaries/${ARCH}"
mkdir -p "${OUTPUT_DIR}"
cc -std=gnu17 -Os -ffunction-sections -fdata-sections -flto \
    -I"${SQLITE_DIR}" \
    -o "${OUTPUT_DIR}/outershelld" \
    "${REPO_ROOT}/Backend/OuterShellBuffer.c" \
    "${REPO_ROOT}/Backend/OuterShellAPI.c" \
    "${REPO_ROOT}/Backend/OuterShellPlatform.c" \
    "${REPO_ROOT}/outershelld/outershelld.c" \
    "${SQLITE_DIR}/sqlite3.c" \
    -Wl,--gc-sections \
    -ldl -lpthread -lm

cc -std=gnu17 -Os -ffunction-sections -fdata-sections -flto \
    -DOUTER_SHELL_BACKEND_STANDALONE=1 \
    -DOUTER_SHELL_USE_LIBCURL=1 \
    "${LIBCURL_CFLAGS[@]}" \
    -Wl,--gc-sections \
    -o "${OUTPUT_DIR}/OuterShellBackend" \
    "${REPO_ROOT}/Backend/OuterShellBuffer.c" \
    "${REPO_ROOT}/Backend/OuterShellAPI.c" \
    "${REPO_ROOT}/Backend/OuterShellPlatform.c" \
    "${REPO_ROOT}/Backend/OuterShellDownloader.c" \
    "${REPO_ROOT}/Backend/OuterShellBackend.c" \
    "${LIBCURL_STATIC_LIB}" \
    -ldl -lpthread -lm \
    "${LIBCURL_LIBS[@]}"

c++ -std=c++17 -Os -ffunction-sections -fdata-sections -flto \
    -Wl,--gc-sections \
    -o "${OUTPUT_DIR}/outerctl" \
    "${RESOURCES_DIR}/outerctl.cpp"

if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${OUTPUT_DIR}/outershelld" || true
    strip --strip-unneeded "${OUTPUT_DIR}/OuterShellBackend" || true
    strip --strip-unneeded "${OUTPUT_DIR}/outerctl" || true
fi

echo "Built Outer Shell Linux resource for ${ARCH}"
