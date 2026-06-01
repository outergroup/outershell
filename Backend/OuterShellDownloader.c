#include "OuterShellDownloader.h"

#if !defined(__APPLE__)

#ifndef OUTER_SHELL_USE_LIBCURL
#error "Linux Outer Shell downloads must be built with OUTER_SHELL_USE_LIBCURL and statically linked libcurl."
#endif

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    size_t limit;
} DownloadTextBuffer;

static void set_error(char *error, size_t error_size, const char *message) {
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", message ? message : "download failed");
    }
}

static bool configure_common(CURL *curl, const char *url, char *error, size_t error_size) {
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "OuterShell/1") != CURLE_OK) {
        set_error(error, error_size, "failed to configure downloader");
        return false;
    }
    static const char *ca_file_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/pki/tls/cacert.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"
    };
    for (size_t i = 0; i < sizeof(ca_file_paths) / sizeof(ca_file_paths[0]); i++) {
        if (access(ca_file_paths[i], R_OK) == 0) {
            if (curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file_paths[i]) != CURLE_OK) {
                set_error(error, error_size, "failed to configure CA bundle");
                return false;
            }
            return true;
        }
    }
    if (access("/etc/ssl/certs", R_OK) == 0 &&
        curl_easy_setopt(curl, CURLOPT_CAPATH, "/etc/ssl/certs") != CURLE_OK) {
        set_error(error, error_size, "failed to configure CA path");
        return false;
    }
    return true;
}

bool outer_shell_download_url_to_file(const char *url,
                                      const char *path,
                                      char *error,
                                      size_t error_size) {
    if (!url || !url[0] || !path || !path[0]) {
        set_error(error, error_size, "missing download URL or path");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        set_error(error, error_size, "failed to initialize libcurl");
        return false;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        curl_easy_cleanup(curl);
        set_error(error, error_size, "failed to open download destination");
        return false;
    }

    char curl_error[CURL_ERROR_SIZE] = "";
    bool ok = configure_common(curl, url, error, error_size) &&
              curl_easy_setopt(curl, CURLOPT_WRITEDATA, file) == CURLE_OK &&
              curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error) == CURLE_OK;
    CURLcode result = ok ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    if (fclose(file) != 0 && result == CURLE_OK) {
        result = CURLE_WRITE_ERROR;
    }
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        set_error(error, error_size, curl_error[0] ? curl_error : curl_easy_strerror(result));
        remove(path);
        return false;
    }
    return true;
}

static size_t write_text_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    DownloadTextBuffer *buffer = (DownloadTextBuffer *)userdata;
    size_t byte_count = size * nmemb;
    if (byte_count == 0) return 0;
    if (buffer->length + byte_count > buffer->limit) return 0;
    if (buffer->length + byte_count + 1 > buffer->capacity) {
        size_t next_capacity = buffer->capacity ? buffer->capacity : 4096;
        while (next_capacity < buffer->length + byte_count + 1) {
            next_capacity *= 2;
        }
        char *next = (char *)realloc(buffer->data, next_capacity);
        if (!next) return 0;
        buffer->data = next;
        buffer->capacity = next_capacity;
    }
    memcpy(buffer->data + buffer->length, ptr, byte_count);
    buffer->length += byte_count;
    buffer->data[buffer->length] = '\0';
    return byte_count;
}

bool outer_shell_fetch_url_text(const char *url,
                                char *out,
                                size_t out_size,
                                char *error,
                                size_t error_size) {
    if (out && out_size > 0) out[0] = '\0';
    if (!url || !url[0] || !out || out_size == 0) {
        set_error(error, error_size, "missing URL or output buffer");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        set_error(error, error_size, "failed to initialize libcurl");
        return false;
    }

    DownloadTextBuffer buffer = {.limit = out_size > 1 ? out_size - 1 : 0};
    char curl_error[CURL_ERROR_SIZE] = "";
    bool ok = configure_common(curl, url, error, error_size) &&
              curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_text_callback) == CURLE_OK &&
              curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer) == CURLE_OK &&
              curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error) == CURLE_OK;
    CURLcode result = ok ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        set_error(error, error_size, curl_error[0] ? curl_error : curl_easy_strerror(result));
        free(buffer.data);
        return false;
    }
    snprintf(out, out_size, "%s", buffer.data ? buffer.data : "");
    free(buffer.data);
    return true;
}

#endif
