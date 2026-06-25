#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OuterShellPaths.h"

namespace {

constexpr uint32_t kOuterctlApiMaxFrameLength = 16u * 1024u * 1024u;
constexpr uint32_t kOpenerCapabilityView = 0x01;
constexpr uint32_t kOpenerCapabilityEdit = 0x02;
constexpr uint32_t kOpenerCapabilityDefault = kOpenerCapabilityView | kOpenerCapabilityEdit;

struct Buffer {
    char *data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
};

void freeBuffer(Buffer &buffer) {
    free(buffer.data);
    buffer.data = nullptr;
    buffer.size = 0;
    buffer.capacity = 0;
}

bool reserveBuffer(Buffer &buffer, size_t capacity) {
    if (capacity <= buffer.capacity) return true;
    size_t newCapacity = buffer.capacity == 0 ? 64 : buffer.capacity;
    while (newCapacity < capacity) {
        if (newCapacity > SIZE_MAX / 2) {
            newCapacity = capacity;
            break;
        }
        newCapacity *= 2;
    }
    char *newData = static_cast<char *>(realloc(buffer.data, newCapacity + 1));
    if (!newData) return false;
    buffer.data = newData;
    buffer.capacity = newCapacity;
    buffer.data[buffer.size] = '\0';
    return true;
}

bool appendBuffer(Buffer &buffer, const void *data, size_t size) {
    if (size == 0) return true;
    if (!reserveBuffer(buffer, buffer.size + size)) return false;
    memcpy(buffer.data + buffer.size, data, size);
    buffer.size += size;
    buffer.data[buffer.size] = '\0';
    return true;
}

bool appendCString(Buffer &buffer, const char *value) {
    return appendBuffer(buffer, value ? value : "", strlen(value ? value : ""));
}

bool appendZeroBytes(Buffer &buffer, size_t size) {
    if (!reserveBuffer(buffer, buffer.size + size)) return false;
    memset(buffer.data + buffer.size, 0, size);
    buffer.size += size;
    buffer.data[buffer.size] = '\0';
    return true;
}

bool assignCString(Buffer &buffer, const char *value) {
    buffer.size = 0;
    if (buffer.data) buffer.data[0] = '\0';
    return appendCString(buffer, value);
}

bool setErrnoError(Buffer &errorMessage, const char *prefix) {
    errorMessage.size = 0;
    if (errorMessage.data) errorMessage.data[0] = '\0';
    return appendCString(errorMessage, prefix) && appendCString(errorMessage, strerror(errno));
}

uint16_t readLittleEndianUInt16(const uint8_t *bytes) {
    return static_cast<uint16_t>(bytes[0]) |
        (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readLittleEndianUInt32(const uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

void writeLittleEndianUInt16(uint8_t *bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value & 0xff);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void writeLittleEndianUInt32(uint8_t *bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value & 0xff);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    bytes[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

bool appendLittleEndianUInt16(Buffer &buffer, uint16_t value) {
    uint8_t bytes[2];
    writeLittleEndianUInt16(bytes, value);
    return appendBuffer(buffer, bytes, sizeof(bytes));
}

bool appendLittleEndianUInt32(Buffer &buffer, uint32_t value) {
    uint8_t bytes[4];
    writeLittleEndianUInt32(bytes, value);
    return appendBuffer(buffer, bytes, sizeof(bytes));
}

bool writeBufferLittleEndianUInt32At(Buffer &buffer, size_t offset, uint32_t value) {
    if (offset + 4 > buffer.size) return false;
    writeLittleEndianUInt32(reinterpret_cast<uint8_t *>(buffer.data + offset), value);
    return true;
}

bool appendOuterctlApiStringRef(Buffer &message, size_t refOffset, const char *value) {
    const char *safeValue = value ? value : "";
    const size_t length = strlen(safeValue);
    if (message.size > UINT32_MAX || length > UINT32_MAX) return false;
    const uint32_t offset = static_cast<uint32_t>(message.size);
    return appendBuffer(message, safeValue, length) &&
        writeBufferLittleEndianUInt32At(message, refOffset, offset) &&
        writeBufferLittleEndianUInt32At(message, refOffset + 4, static_cast<uint32_t>(length));
}

bool appendOuterctlApiStringRefBytes(Buffer &message, size_t refOffset, const char *value, size_t length) {
    if (message.size > UINT32_MAX || length > UINT32_MAX) return false;
    const uint32_t offset = static_cast<uint32_t>(message.size);
    return appendBuffer(message, value ? value : "", length) &&
        writeBufferLittleEndianUInt32At(message, refOffset, offset) &&
        writeBufferLittleEndianUInt32At(message, refOffset + 4, static_cast<uint32_t>(length));
}

bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool nextCsvToken(const char *&cursor, const char *&data, size_t &size) {
    if (!cursor) return false;
    while (*cursor) {
        while (*cursor == ',' || isAsciiSpace(*cursor)) cursor += 1;
        if (!*cursor) return false;
        const char *start = cursor;
        const char *end = cursor;
        while (*end && *end != ',') end += 1;
        const char *trimEnd = end;
        while (trimEnd > start && isAsciiSpace(trimEnd[-1])) trimEnd -= 1;
        cursor = end;
        if (*cursor == ',') cursor += 1;
        if (trimEnd > start) {
            data = start;
            size = static_cast<size_t>(trimEnd - start);
            return true;
        }
    }
    return false;
}

size_t countCsvTokens(const char *csv) {
    const char *cursor = csv ? csv : "";
    const char *data = "";
    size_t size = 0;
    size_t count = 0;
    while (nextCsvToken(cursor, data, size)) count += 1;
    return count;
}

bool appendOuterctlApiStringListRef(Buffer &message, size_t refOffset, const char *csv) {
    const size_t count = countCsvTokens(csv);
    if (count == 0) {
        return writeBufferLittleEndianUInt32At(message, refOffset, 0) &&
            writeBufferLittleEndianUInt32At(message, refOffset + 4, 0);
    }
    if (count > UINT32_MAX || count > SIZE_MAX / 8 || message.size > UINT32_MAX) return false;
    const size_t listOffset = message.size;
    if (!writeBufferLittleEndianUInt32At(message, refOffset, static_cast<uint32_t>(listOffset)) ||
        !writeBufferLittleEndianUInt32At(message, refOffset + 4, static_cast<uint32_t>(count)) ||
        !appendZeroBytes(message, count * 8)) {
        return false;
    }
    const char *cursor = csv ? csv : "";
    const char *data = "";
    size_t size = 0;
    size_t index = 0;
    while (nextCsvToken(cursor, data, size)) {
        if (!appendOuterctlApiStringRefBytes(message, listOffset + index * 8, data, size)) return false;
        index += 1;
    }
    return index == count;
}

bool readExact(int fd, void *data, size_t size) {
    char *bytes = static_cast<char *>(data);
    while (size > 0) {
        ssize_t got = read(fd, bytes, size);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        bytes += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

bool writeExact(int fd, const void *data, size_t size) {
    const char *bytes = static_cast<const char *>(data);
    while (size > 0) {
        ssize_t wrote = write(fd, bytes, size);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes += wrote;
        size -= static_cast<size_t>(wrote);
    }
    return true;
}

bool readOuterctlApiStringRef(const Buffer &message, size_t refOffset, Buffer &out) {
    if (refOffset + 8 > message.size) return false;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(message.data);
    const uint32_t offset = readLittleEndianUInt32(bytes + refOffset);
    const uint32_t length = readLittleEndianUInt32(bytes + refOffset + 4);
    if (offset > message.size || length > message.size - offset) return false;
    out.size = 0;
    if (out.data) out.data[0] = '\0';
    return appendBuffer(out, message.data + offset, length);
}

bool readOuterctlApiStringRefView(const Buffer &message, size_t refOffset, const char *&data, size_t &size) {
    if (refOffset + 8 > message.size) return false;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(message.data);
    const uint32_t offset = readLittleEndianUInt32(bytes + refOffset);
    const uint32_t length = readLittleEndianUInt32(bytes + refOffset + 4);
    if (offset > message.size || length > message.size - offset) return false;
    data = message.data + offset;
    size = length;
    return true;
}

bool writeTsvField(const char *data, size_t size) {
    for (size_t i = 0; i < size; i += 1) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == '\t') {
            if (fputs("\\t", stdout) < 0) return false;
        } else if (c == '\n') {
            if (fputs("\\n", stdout) < 0) return false;
        } else if (c == '\r') {
            if (fputs("\\r", stdout) < 0) return false;
        } else if (c == '\\') {
            if (fputs("\\\\", stdout) < 0) return false;
        } else if (fputc(c, stdout) == EOF) {
            return false;
        }
    }
    return true;
}

bool writeTsvString(const char *value) {
    const char *safeValue = value ? value : "";
    return writeTsvField(safeValue, strlen(safeValue));
}

bool writeOpenerCapabilities(uint32_t capabilities) {
    bool wrote = false;
    if (capabilities & kOpenerCapabilityView) {
        if (!writeTsvString("view")) return false;
        wrote = true;
    }
    if (capabilities & kOpenerCapabilityEdit) {
        if (wrote && fputc(',', stdout) == EOF) return false;
        if (!writeTsvString("edit")) return false;
        wrote = true;
    }
    return wrote || writeTsvString("");
}

bool writeTsvHeaders(const char *const *headers, size_t count) {
    for (size_t i = 0; i < count; i += 1) {
        if (i > 0 && fputc('\t', stdout) == EOF) return false;
        if (!writeTsvString(headers[i])) return false;
    }
    return fputc('\n', stdout) != EOF;
}

bool writeTsvRef(const Buffer &message, size_t refOffset) {
    const char *data = "";
    size_t size = 0;
    return readOuterctlApiStringRefView(message, refOffset, data, size) &&
        writeTsvField(data, size);
}

bool writeTsvStringListRef(const Buffer &message, size_t refOffset) {
    if (refOffset + 8 > message.size) return false;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(message.data);
    const uint32_t listOffset = readLittleEndianUInt32(bytes + refOffset);
    const uint32_t count = readLittleEndianUInt32(bytes + refOffset + 4);
    if (count == 0) return true;
    if (listOffset == 0 || listOffset > message.size) return false;
    if (count > (message.size - listOffset) / 8) return false;
    for (uint32_t i = 0; i < count; i += 1) {
        if (i > 0 && fputc(',', stdout) == EOF) return false;
        if (!writeTsvRef(message, listOffset + static_cast<size_t>(i) * 8)) return false;
    }
    return true;
}

bool writeTsvRefs(const Buffer &message, size_t rowOffset, const size_t *refs, size_t count) {
    for (size_t i = 0; i < count; i += 1) {
        if (i > 0 && fputc('\t', stdout) == EOF) return false;
        if (!writeTsvRef(message, rowOffset + refs[i])) return false;
    }
    return true;
}

bool writeTsvRefRow(const Buffer &message, size_t rowOffset, const size_t *refs, size_t count) {
    return writeTsvRefs(message, rowOffset, refs, count) &&
        fputc('\n', stdout) != EOF;
}

void defaultOuterctlApiSocketPath(char *out, size_t outSize) {
    outer_shell_default_api_socket_path(out, outSize);
}

enum : uint16_t {
    kMessageBackendUpsertRequest = 10,
    kMessageBackendRemoveRequest = 11,
    kMessageBackendListRequest = 12,
    kMessageAppAddRequest = 13,
    kMessageAppRemoveRequest = 14,
    kMessageAppListRequest = 15,
    kMessageLogAddRequest = 16,
    kMessageLogRemoveRequest = 17,
    kMessageLogListRequest = 18,
    kMessageContentTypeAddRequest = 19,
    kMessageContentTypeRemoveRequest = 20,
    kMessageContentTypeListRequest = 21,
    kMessageOpenerAddRequest = 22,
    kMessageOpenerRemoveRequest = 23,
    kMessageOpenerListRequest = 24,
    kMessageCommandResponse = 100,
    kMessageBackendListResponse = 101,
    kMessageAppListResponse = 102,
    kMessageLogListResponse = 103,
    kMessageContentTypeListResponse = 104,
    kMessageOpenerListResponse = 105
};

enum : uint16_t {
    kFlagOwnsServiceManagerEntry = 0x01,
    kFlagIncludeIcons = 0x02
};

enum : uint16_t {
    kFrontendEndpointNone = 0,
    kFrontendEndpointTcp = 1,
    kFrontendEndpointUnix = 2
};

enum : uint16_t {
    kFrontendSchemeDefault = 0,
    kFrontendSchemeHttp = 1,
    kFrontendSchemeHttps = 2
};

struct CommandRequest {
    uint16_t messageType = 0;
    uint16_t flags = 0;
    uint16_t endpointKind = kFrontendEndpointNone;
    uint16_t endpointScheme = kFrontendSchemeHttp;
    uint16_t endpointFlags = 0;
    uint32_t port = 0;
    uint32_t rank = 0;
    uint32_t openerCapabilities = kOpenerCapabilityDefault;
    bool hasServiceManagerPath = false;
    const char *backend = "";
    const char *displayName = "";
    const char *serviceManagerPath = "";
    const char *logPath = "";
    const char *url = "";
    const char *host = "";
    const char *path = "";
    const char *frontendId = "";
    const char *iconPath = "";
    const char *frontendList = "";
    const char *socketPath = "";
    const char *contentType = "";
    const char *conformsTo = "";
    const char *extensions = "";
    const char *filenames = "";
    const char *mimeTypes = "";
    const char *urlTemplate = "";
};

bool parseUInt32(const char *raw, uint32_t maxValue, uint32_t &out) {
    if (!raw || !raw[0]) return false;
    char *end = nullptr;
    unsigned long value = strtoul(raw, &end, 10);
    if (!end || *end != '\0' || value > maxValue) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool truthy(const char *raw) {
    return raw && (strcmp(raw, "true") == 0 || strcmp(raw, "1") == 0 || strcmp(raw, "yes") == 0);
}

void normalizePath(const char *raw, Buffer &out) {
    out.size = 0;
    if (out.data) out.data[0] = '\0';
    const char *path = raw && raw[0] ? raw : "/";
    if (path[0] == '/') {
        appendCString(out, path);
    } else {
        appendCString(out, "/");
        appendCString(out, path);
    }
}

const char *firstUrlSuffix(const char *authority) {
    const char *slash = strchr(authority, '/');
    const char *query = strchr(authority, '?');
    const char *fragment = strchr(authority, '#');
    const char *suffix = slash;
    if (!suffix || (query && query < suffix)) suffix = query;
    if (!suffix || (fragment && fragment < suffix)) suffix = fragment;
    return suffix;
}

bool parseHostPort(const char *start, size_t length, Buffer &host, uint32_t &port) {
    const char *hostEnd = start + length;
    const char *portStart = nullptr;
    if (length > 0 && start[0] == '[') {
        const char *closing = static_cast<const char *>(memchr(start, ']', length));
        if (closing) {
            hostEnd = closing + 1;
            if (static_cast<size_t>(hostEnd - start) < length && *hostEnd == ':') {
                portStart = hostEnd + 1;
            }
        }
    } else {
        const char *colon = nullptr;
        for (const char *cursor = start; cursor < start + length; cursor += 1) {
            if (*cursor == ':') colon = cursor;
        }
        if (colon) {
            hostEnd = colon;
            portStart = colon + 1;
        }
    }
    host.size = 0;
    if (host.data) host.data[0] = '\0';
    if (hostEnd > start) {
        if (!appendBuffer(host, start, static_cast<size_t>(hostEnd - start))) return false;
    }
    if (portStart && portStart < start + length) {
        char buffer[16];
        size_t portLength = static_cast<size_t>((start + length) - portStart);
        if (portLength >= sizeof(buffer)) portLength = sizeof(buffer) - 1;
        memcpy(buffer, portStart, portLength);
        buffer[portLength] = '\0';
        uint32_t parsed = 0;
        if (parseUInt32(buffer, 65535, parsed) && parsed > 0) port = parsed;
    }
    return true;
}

bool parseAppEndpoint(CommandRequest &request, Buffer &hostBuffer, Buffer &pathBuffer, Buffer &errorMessage) {
    bool hasSocket = request.socketPath && request.socketPath[0];
    bool hasPort = request.port > 0;
    request.endpointKind = hasSocket ? kFrontendEndpointUnix : (hasPort ? kFrontendEndpointTcp : kFrontendEndpointNone);
    if (request.endpointScheme == kFrontendSchemeDefault) request.endpointScheme = kFrontendSchemeHttp;
    assignCString(hostBuffer, "127.0.0.1");
    normalizePath("/", pathBuffer);

    const char *url = request.url ? request.url : "";
    if (url[0]) {
        const char *authority = url;
        const char *scheme = strstr(url, "://");
        if (scheme) {
            size_t schemeLength = static_cast<size_t>(scheme - url);
            if (schemeLength == 5 && strncmp(url, "https", 5) == 0) request.endpointScheme = kFrontendSchemeHttps;
            else if (schemeLength == 4 && strncmp(url, "http", 4) == 0) request.endpointScheme = kFrontendSchemeHttp;
            else {
                assignCString(errorMessage, "Invalid app endpoint URL scheme.");
                return false;
            }
            authority = scheme + 3;
        }
        if (url[0] == '/' || url[0] == '?' || url[0] == '#') {
            normalizePath(url, pathBuffer);
        } else {
            const char *suffix = firstUrlSuffix(authority);
            const char *authorityEnd = suffix ? suffix : authority + strlen(authority);
            bool looksLikeAuthority = scheme != nullptr ||
                memchr(authority, ':', static_cast<size_t>(authorityEnd - authority)) != nullptr;
            if (looksLikeAuthority) {
                if (!hasSocket && !parseHostPort(authority, static_cast<size_t>(authorityEnd - authority), hostBuffer, request.port)) {
                    assignCString(errorMessage, "Invalid app endpoint host.");
                    return false;
                }
                normalizePath(suffix ? suffix : "/", pathBuffer);
                if (!hasSocket) request.endpointKind = kFrontendEndpointTcp;
            } else {
                normalizePath(url, pathBuffer);
            }
        }
    }
    if (request.host && request.host[0]) {
        if (hasSocket) {
            assignCString(errorMessage, "Specify --host only with TCP endpoints.");
            return false;
        }
        assignCString(hostBuffer, request.host);
        request.endpointKind = kFrontendEndpointTcp;
    }
    if (request.path && request.path[0]) normalizePath(request.path, pathBuffer);
    if (hasPort) request.endpointKind = kFrontendEndpointTcp;
    if (hasSocket) request.endpointKind = kFrontendEndpointUnix;
    request.host = hostBuffer.data ? hostBuffer.data : "";
    request.path = pathBuffer.data ? pathBuffer.data : "/";
    return true;
}

bool parseOpenerCapabilities(const char *raw, uint32_t &out) {
    if (!raw || !raw[0]) return false;
    uint32_t flags = 0;
    const char *cursor = raw;
    while (*cursor) {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? static_cast<size_t>(comma - cursor) : strlen(cursor);
        if (length == 4 && strncmp(cursor, "view", length) == 0) {
            flags |= kOpenerCapabilityView;
        } else if (length == 6 && strncmp(cursor, "viewer", length) == 0) {
            flags |= kOpenerCapabilityView;
        } else if (length == 4 && strncmp(cursor, "edit", length) == 0) {
            flags |= kOpenerCapabilityEdit;
        } else if (length == 6 && strncmp(cursor, "editor", length) == 0) {
            flags |= kOpenerCapabilityEdit;
        } else {
            return false;
        }
        if (!comma) break;
        cursor = comma + 1;
        if (!*cursor) return false;
    }
    if ((flags & kOpenerCapabilityDefault) == 0) return false;
    out = flags;
    return true;
}

bool mapCommand(const char *resource, const char *action, uint16_t &messageType) {
    if (strcmp(resource, "backend") == 0) {
        if (strcmp(action, "upsert") == 0) messageType = kMessageBackendUpsertRequest;
        else if (strcmp(action, "remove") == 0) messageType = kMessageBackendRemoveRequest;
        else if (strcmp(action, "list") == 0) messageType = kMessageBackendListRequest;
        else return false;
    } else if (strcmp(resource, "app") == 0) {
        if (strcmp(action, "add") == 0) messageType = kMessageAppAddRequest;
        else if (strcmp(action, "remove") == 0) messageType = kMessageAppRemoveRequest;
        else if (strcmp(action, "list") == 0) messageType = kMessageAppListRequest;
        else return false;
    } else if (strcmp(resource, "log") == 0) {
        if (strcmp(action, "add") == 0) messageType = kMessageLogAddRequest;
        else if (strcmp(action, "remove") == 0) messageType = kMessageLogRemoveRequest;
        else if (strcmp(action, "list") == 0) messageType = kMessageLogListRequest;
        else return false;
    } else if (strcmp(resource, "content-type") == 0 || strcmp(resource, "type") == 0) {
        if (strcmp(action, "add") == 0) messageType = kMessageContentTypeAddRequest;
        else if (strcmp(action, "remove") == 0) messageType = kMessageContentTypeRemoveRequest;
        else if (strcmp(action, "list") == 0) messageType = kMessageContentTypeListRequest;
        else return false;
    } else if (strcmp(resource, "opener") == 0) {
        if (strcmp(action, "add") == 0) messageType = kMessageOpenerAddRequest;
        else if (strcmp(action, "remove") == 0) messageType = kMessageOpenerRemoveRequest;
        else if (strcmp(action, "list") == 0) messageType = kMessageOpenerListRequest;
        else return false;
    } else {
        return false;
    }
    return true;
}

bool parseCommandRequest(int argc, char *argv[], CommandRequest &request, Buffer &errorMessage) {
    if (argc < 3) {
        assignCString(errorMessage, "Usage: outerctl <resource> <action> [options]");
        return false;
    }
    if (!mapCommand(argv[1], argv[2], request.messageType)) {
        assignCString(errorMessage, "Unknown outerctl command.");
        return false;
    }

    for (int i = 3; i < argc; i += 1) {
        const char *arg = argv[i];
#define REQUIRE_VALUE(name, target) do { \
            if (i + 1 >= argc) { \
                assignCString(errorMessage, "Missing value for " name); \
                return false; \
            } \
            target = argv[++i]; \
        } while (0)
        if (strcmp(arg, "--backend") == 0) {
            REQUIRE_VALUE("--backend", request.backend);
        } else if (strcmp(arg, "--name") == 0) {
            REQUIRE_VALUE("--name", request.displayName);
        } else if (strcmp(arg, "--plist") == 0 || strcmp(arg, "--launchd-plist") == 0) {
            if (request.hasServiceManagerPath) {
                assignCString(errorMessage, "Specify only one service-manager value.");
                return false;
            }
            REQUIRE_VALUE("--plist", request.serviceManagerPath);
            request.hasServiceManagerPath = true;
        } else if (strcmp(arg, "--unit") == 0 || strcmp(arg, "--systemd-unit") == 0) {
            if (request.hasServiceManagerPath) {
                assignCString(errorMessage, "Specify only one service-manager value.");
                return false;
            }
            REQUIRE_VALUE("--unit", request.serviceManagerPath);
            request.hasServiceManagerPath = true;
        } else if (strcmp(arg, "--path") == 0) {
            if (strcmp(argv[1], "app") == 0) {
                REQUIRE_VALUE("--path", request.path);
            } else {
                REQUIRE_VALUE("--path", request.logPath);
            }
        } else if (strcmp(arg, "--url") == 0) {
            REQUIRE_VALUE("--url", request.url);
        } else if (strcmp(arg, "--host") == 0) {
            REQUIRE_VALUE("--host", request.host);
        } else if (strcmp(arg, "--scheme") == 0) {
            const char *rawScheme = nullptr;
            REQUIRE_VALUE("--scheme", rawScheme);
            if (strcmp(rawScheme, "https") == 0) request.endpointScheme = kFrontendSchemeHttps;
            else if (strcmp(rawScheme, "http") == 0) request.endpointScheme = kFrontendSchemeHttp;
            else {
                assignCString(errorMessage, "Invalid app endpoint scheme.");
                return false;
            }
        } else if (strcmp(arg, "--frontend-id") == 0 || strcmp(arg, "--id") == 0) {
            REQUIRE_VALUE("--frontend-id", request.frontendId);
        } else if (strcmp(arg, "--icon-path") == 0 || strcmp(arg, "--icon-file") == 0) {
            REQUIRE_VALUE("--icon-path", request.iconPath);
        } else if (strcmp(arg, "--list") == 0) {
            REQUIRE_VALUE("--list", request.frontendList);
        } else if (strcmp(arg, "--socket-path") == 0) {
            REQUIRE_VALUE("--socket-path", request.socketPath);
        } else if (strcmp(arg, "--content-type") == 0 || strcmp(arg, "--type") == 0) {
            REQUIRE_VALUE("--content-type", request.contentType);
        } else if (strcmp(arg, "--conforms-to") == 0) {
            REQUIRE_VALUE("--conforms-to", request.conformsTo);
        } else if (strcmp(arg, "--extensions") == 0) {
            REQUIRE_VALUE("--extensions", request.extensions);
        } else if (strcmp(arg, "--filenames") == 0) {
            REQUIRE_VALUE("--filenames", request.filenames);
        } else if (strcmp(arg, "--mime-types") == 0) {
            REQUIRE_VALUE("--mime-types", request.mimeTypes);
        } else if (strcmp(arg, "--url-template") == 0) {
            REQUIRE_VALUE("--url-template", request.urlTemplate);
        } else if (strcmp(arg, "--capabilities") == 0) {
            const char *rawCapabilities = nullptr;
            REQUIRE_VALUE("--capabilities", rawCapabilities);
            if (!parseOpenerCapabilities(rawCapabilities, request.openerCapabilities)) {
                assignCString(errorMessage, "Invalid opener capabilities.");
                return false;
            }
        } else if (strcmp(arg, "--port") == 0) {
            const char *rawPort = nullptr;
            REQUIRE_VALUE("--port", rawPort);
            uint32_t port = 0;
            if (!parseUInt32(rawPort, 65535, port) || port == 0) {
                assignCString(errorMessage, "Invalid port.");
                return false;
            }
            request.port = port;
        } else if (strcmp(arg, "--rank") == 0) {
            const char *rawRank = nullptr;
            REQUIRE_VALUE("--rank", rawRank);
            uint32_t rank = 0;
            if (!parseUInt32(rawRank, INT32_MAX, rank)) {
                assignCString(errorMessage, "Invalid rank.");
                return false;
            }
            request.rank = rank;
        } else if (strcmp(arg, "--outershell-owns") == 0) {
            const char *raw = nullptr;
            REQUIRE_VALUE("--outershell-owns", raw);
            if (truthy(raw)) request.flags |= kFlagOwnsServiceManagerEntry;
        } else if (strcmp(arg, "--icons") == 0) {
            request.flags |= kFlagIncludeIcons;
        } else {
            errorMessage.size = 0;
            if (errorMessage.data) errorMessage.data[0] = '\0';
            appendCString(errorMessage, "Unknown argument: ");
            appendCString(errorMessage, arg);
            return false;
        }
#undef REQUIRE_VALUE
    }
    return true;
}

bool appendStringRefs(Buffer &message, const char *const *values, size_t count) {
    const size_t refsOffset = message.size;
    if (!appendZeroBytes(message, count * 8)) return false;
    for (size_t i = 0; i < count; i += 1) {
        if (!appendOuterctlApiStringRef(message, refsOffset + i * 8, values[i])) return false;
    }
    return true;
}

bool appendCommandRequestMessage(Buffer &message, const CommandRequest &request) {
    bool ok = appendLittleEndianUInt16(message, request.messageType);
    switch (request.messageType) {
    case kMessageBackendUpsertRequest: {
        const char *values[] = {request.backend, request.displayName, request.serviceManagerPath};
        ok = ok && appendLittleEndianUInt16(message, request.flags) && appendStringRefs(message, values, 3);
        break;
    }
    case kMessageBackendRemoveRequest:
    case kMessageBackendListRequest:
    case kMessageAppListRequest:
    case kMessageLogListRequest: {
        const char *values[] = {request.backend};
        ok = ok && appendStringRefs(message, values, 1);
        break;
    }
    case kMessageAppAddRequest: {
        const char *values[] = {request.backend, request.frontendId, request.displayName, request.path, request.host, request.socketPath, request.iconPath, request.frontendList};
        ok = ok &&
            appendLittleEndianUInt16(message, request.endpointKind) &&
            appendLittleEndianUInt16(message, request.endpointScheme) &&
            appendLittleEndianUInt16(message, request.endpointFlags) &&
            appendLittleEndianUInt16(message, static_cast<uint16_t>(request.port)) &&
            appendStringRefs(message, values, 8);
        break;
    }
    case kMessageAppRemoveRequest: {
        const char *values[] = {request.backend, request.frontendId, request.socketPath};
        ok = ok && appendLittleEndianUInt32(message, request.port) && appendStringRefs(message, values, 3);
        break;
    }
    case kMessageLogAddRequest:
    case kMessageLogRemoveRequest: {
        const char *values[] = {request.backend, request.logPath};
        ok = ok && appendStringRefs(message, values, 2);
        break;
    }
    case kMessageContentTypeAddRequest: {
        const size_t fieldsOffset = message.size;
        ok = ok &&
            appendZeroBytes(message, 56) &&
            appendOuterctlApiStringRef(message, fieldsOffset, request.backend) &&
            appendOuterctlApiStringRef(message, fieldsOffset + 8, request.contentType) &&
            appendOuterctlApiStringRef(message, fieldsOffset + 16, request.displayName) &&
            appendOuterctlApiStringListRef(message, fieldsOffset + 24, request.conformsTo) &&
            appendOuterctlApiStringListRef(message, fieldsOffset + 32, request.extensions) &&
            appendOuterctlApiStringListRef(message, fieldsOffset + 40, request.filenames) &&
            appendOuterctlApiStringListRef(message, fieldsOffset + 48, request.mimeTypes);
        break;
    }
    case kMessageContentTypeRemoveRequest:
    case kMessageContentTypeListRequest: {
        const char *values[] = {request.backend, request.contentType};
        ok = ok && appendStringRefs(message, values, 2);
        break;
    }
    case kMessageOpenerAddRequest: {
        const char *values[] = {request.backend, request.contentType, request.displayName, request.socketPath, request.urlTemplate};
        ok = ok &&
            appendLittleEndianUInt32(message, request.rank) &&
            appendLittleEndianUInt32(message, request.openerCapabilities) &&
            appendStringRefs(message, values, 5);
        break;
    }
    case kMessageOpenerRemoveRequest: {
        const char *values[] = {request.backend, request.contentType};
        ok = ok && appendStringRefs(message, values, 2);
        break;
    }
    case kMessageOpenerListRequest: {
        const char *values[] = {request.backend, request.contentType};
        ok = ok && appendStringRefs(message, values, 2);
        break;
    }
    default:
        ok = false;
        break;
    }
    return ok;
}

uint16_t listResponseTypeForRequest(uint16_t messageType) {
    switch (messageType) {
    case kMessageBackendListRequest:
        return kMessageBackendListResponse;
    case kMessageAppListRequest:
        return kMessageAppListResponse;
    case kMessageLogListRequest:
        return kMessageLogListResponse;
    case kMessageContentTypeListRequest:
        return kMessageContentTypeListResponse;
    case kMessageOpenerListRequest:
        return kMessageOpenerListResponse;
    default:
        return 0;
    }
}

bool writeRegistryListResponse(const CommandRequest &request, const Buffer &response, int &exitStatus, Buffer &errorMessage) {
    if (response.size < 22) {
        assignCString(errorMessage, "outershelld API list response frame is invalid.");
        return false;
    }
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(response.data);
    const uint16_t responseType = readLittleEndianUInt16(bytes);
    const uint16_t expectedResponseType = listResponseTypeForRequest(request.messageType);
    if (responseType != expectedResponseType) {
        assignCString(errorMessage, "outershelld API list response type is invalid.");
        return false;
    }

    exitStatus = static_cast<int>(readLittleEndianUInt32(bytes + 2));
    Buffer errorBuffer;
    bool ok = readOuterctlApiStringRef(response, 6, errorBuffer);
    const uint32_t rowCount = readLittleEndianUInt32(bytes + 14);
    const uint32_t responseRowSize = readLittleEndianUInt32(bytes + 18);
    if (!ok) {
        freeBuffer(errorBuffer);
        assignCString(errorMessage, "outershelld API list response payload is invalid.");
        return false;
    }
    if (exitStatus != 0) {
        if (errorBuffer.size > 0) fwrite(errorBuffer.data, 1, errorBuffer.size, stderr);
        if (errorBuffer.size == 0 || errorBuffer.data[errorBuffer.size - 1] != '\n') fputc('\n', stderr);
        freeBuffer(errorBuffer);
        return true;
    }
    freeBuffer(errorBuffer);

    const char *backendHeaders[] = {"service_id", "display_name", "unit_name", "unit_path", "owns_unit"};
    const char *appHeaders[] = {"frontend_id", "service_id", "display_name", "endpoint_kind", "scheme", "host", "port", "socket_path", "path", "url", "icon_path", "list"};
    const char *logHeaders[] = {"path", "service_id"};
    const char *contentTypeHeaders[] = {"service_id", "identifier", "display_name", "conforms_to", "extensions", "filenames", "mime_types"};
    const char *openerHeaders[] = {"content_type", "service_id", "display_name", "socket_path", "url_template", "rank", "capabilities"};

    size_t minimumRowSize = 0;
    size_t rowBase = 22;
    switch (responseType) {
    case kMessageBackendListResponse:
        minimumRowSize = 36;
        if (responseRowSize < minimumRowSize) {
            ok = false;
            break;
        }
        ok = writeTsvHeaders(backendHeaders, sizeof(backendHeaders) / sizeof(backendHeaders[0]));
        for (uint32_t row = 0; ok && row < rowCount; row += 1) {
            const size_t offset = rowBase + static_cast<size_t>(row) * responseRowSize;
            if (offset + minimumRowSize > response.size) {
                ok = false;
                break;
            }
            const size_t refs[] = {4, 12, 20, 28};
            ok = writeTsvRefs(response, offset, refs, sizeof(refs) / sizeof(refs[0])) &&
                fputc('\t', stdout) != EOF &&
                writeTsvString(readLittleEndianUInt32(bytes + offset) ? "1" : "0") &&
                fputc('\n', stdout) != EOF;
        }
        break;
    case kMessageAppListResponse:
        minimumRowSize = 80;
        if (responseRowSize < minimumRowSize) {
            ok = false;
            break;
        }
        ok = writeTsvHeaders(appHeaders, sizeof(appHeaders) / sizeof(appHeaders[0]));
        for (uint32_t row = 0; ok && row < rowCount; row += 1) {
            const size_t offset = rowBase + static_cast<size_t>(row) * responseRowSize;
            if (offset + minimumRowSize > response.size) {
                ok = false;
                break;
            }
            char kindBuffer[16];
            char schemeBuffer[16];
            char portBuffer[32];
            snprintf(kindBuffer, sizeof(kindBuffer), "%u", readLittleEndianUInt16(bytes + offset));
            snprintf(schemeBuffer, sizeof(schemeBuffer), "%u", readLittleEndianUInt16(bytes + offset + 2));
            snprintf(portBuffer, sizeof(portBuffer), "%u", readLittleEndianUInt16(bytes + offset + 6));
            const size_t firstRefs[] = {8, 16, 24};
            const size_t lastRefs[] = {40, 48, 56, 64, 72};
            ok = writeTsvRefs(response, offset, firstRefs, sizeof(firstRefs) / sizeof(firstRefs[0])) &&
                fputc('\t', stdout) != EOF &&
                writeTsvString(kindBuffer) &&
                fputc('\t', stdout) != EOF &&
                writeTsvString(schemeBuffer) &&
                fputc('\t', stdout) != EOF &&
                writeTsvRef(response, offset + 32) &&
                fputc('\t', stdout) != EOF &&
                writeTsvString(portBuffer);
            for (size_t i = 0; ok && i < sizeof(lastRefs) / sizeof(lastRefs[0]); i += 1) {
                ok = fputc('\t', stdout) != EOF && writeTsvRef(response, offset + lastRefs[i]);
            }
            if (ok) ok = fputc('\n', stdout) != EOF;
        }
        break;
    case kMessageLogListResponse:
        minimumRowSize = 16;
        if (responseRowSize < minimumRowSize) {
            ok = false;
            break;
        }
        ok = writeTsvHeaders(logHeaders, sizeof(logHeaders) / sizeof(logHeaders[0]));
        for (uint32_t row = 0; ok && row < rowCount; row += 1) {
            const size_t offset = rowBase + static_cast<size_t>(row) * responseRowSize;
            if (offset + minimumRowSize > response.size) {
                ok = false;
                break;
            }
            const size_t refs[] = {0, 8};
            ok = writeTsvRefRow(response, offset, refs, sizeof(refs) / sizeof(refs[0]));
        }
        break;
    case kMessageContentTypeListResponse:
        minimumRowSize = 56;
        if (responseRowSize < minimumRowSize) {
            ok = false;
            break;
        }
        ok = writeTsvHeaders(contentTypeHeaders, sizeof(contentTypeHeaders) / sizeof(contentTypeHeaders[0]));
        for (uint32_t row = 0; ok && row < rowCount; row += 1) {
            const size_t offset = rowBase + static_cast<size_t>(row) * responseRowSize;
            if (offset + minimumRowSize > response.size) {
                ok = false;
                break;
            }
            const size_t refs[] = {0, 8, 16};
            const size_t lists[] = {24, 32, 40, 48};
            ok = writeTsvRefs(response, offset, refs, sizeof(refs) / sizeof(refs[0]));
            for (size_t i = 0; ok && i < sizeof(lists) / sizeof(lists[0]); i += 1) {
                ok = fputc('\t', stdout) != EOF && writeTsvStringListRef(response, offset + lists[i]);
            }
            if (ok) ok = fputc('\n', stdout) != EOF;
        }
        break;
    case kMessageOpenerListResponse:
        minimumRowSize = 48;
        if (responseRowSize < minimumRowSize) {
            ok = false;
            break;
        }
        ok = writeTsvHeaders(openerHeaders, sizeof(openerHeaders) / sizeof(openerHeaders[0]));
        for (uint32_t row = 0; ok && row < rowCount; row += 1) {
            const size_t offset = rowBase + static_cast<size_t>(row) * responseRowSize;
            if (offset + minimumRowSize > response.size) {
                ok = false;
                break;
            }
            char rankBuffer[32];
            snprintf(rankBuffer, sizeof(rankBuffer), "%u", readLittleEndianUInt32(bytes + offset));
            const size_t refs[] = {4, 12, 20, 28, 36};
            ok = writeTsvRefs(response, offset, refs, sizeof(refs) / sizeof(refs[0])) &&
                fputc('\t', stdout) != EOF &&
                writeTsvString(rankBuffer) &&
                fputc('\t', stdout) != EOF &&
                writeOpenerCapabilities(readLittleEndianUInt32(bytes + offset + 44)) &&
                fputc('\n', stdout) != EOF;
        }
        break;
    default:
        ok = false;
        break;
    }
    if (!ok) assignCString(errorMessage, "outershelld API list response payload is invalid.");
    return ok;
}

bool tryOuterctlApi(const CommandRequest &request, int &exitStatus, Buffer &errorMessage) {
    char socketPath[PATH_MAX];
    defaultOuterctlApiSocketPath(socketPath, sizeof(socketPath));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        setErrnoError(errorMessage, "Failed to create API socket: ");
        return false;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (strlen(socketPath) >= sizeof(address.sun_path)) {
        close(fd);
        assignCString(errorMessage, "outershelld API socket path is too long.");
        return false;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketPath);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        errorMessage.size = 0;
        if (errorMessage.data) errorMessage.data[0] = '\0';
        appendCString(errorMessage, "Failed to connect to outershelld API socket ");
        appendCString(errorMessage, socketPath);
        appendCString(errorMessage, ": ");
        appendCString(errorMessage, strerror(errno));
        close(fd);
        return false;
    }

    Buffer message;
    bool ok = appendCommandRequestMessage(message, request);
    if (!ok || message.size > UINT32_MAX) {
        freeBuffer(message);
        close(fd);
        assignCString(errorMessage, "Failed to encode outershelld API request.");
        return false;
    }

    uint8_t lengthPrefix[4];
    writeLittleEndianUInt32(lengthPrefix, static_cast<uint32_t>(message.size));
    ok = writeExact(fd, lengthPrefix, sizeof(lengthPrefix)) &&
        writeExact(fd, message.data, message.size);
    freeBuffer(message);
    if (!ok) {
        close(fd);
        setErrnoError(errorMessage, "Failed to write outershelld API request: ");
        return false;
    }

    uint8_t responseLengthBytes[4];
    if (!readExact(fd, responseLengthBytes, sizeof(responseLengthBytes))) {
        close(fd);
        setErrnoError(errorMessage, "Failed to read outershelld API response header: ");
        return false;
    }
    const uint32_t responseLength = readLittleEndianUInt32(responseLengthBytes);
    if (responseLength < 18 || responseLength > kOuterctlApiMaxFrameLength) {
        close(fd);
        assignCString(errorMessage, "outershelld API response frame is invalid.");
        return false;
    }

    Buffer response;
    ok = reserveBuffer(response, responseLength) && readExact(fd, response.data, responseLength);
    close(fd);
    if (!ok) {
        freeBuffer(response);
        setErrnoError(errorMessage, "Failed to read outershelld API response: ");
        return false;
    }
    response.size = responseLength;
    response.data[response.size] = '\0';

    const uint8_t *responseBytes = reinterpret_cast<const uint8_t *>(response.data);
    const uint16_t responseType = readLittleEndianUInt16(responseBytes);
    const uint16_t expectedListResponseType = listResponseTypeForRequest(request.messageType);
    if (expectedListResponseType != 0) {
        ok = writeRegistryListResponse(request, response, exitStatus, errorMessage);
        freeBuffer(response);
        return ok;
    }
    if (responseType != kMessageCommandResponse) {
        freeBuffer(response);
        assignCString(errorMessage, "outershelld API response type is invalid.");
        return false;
    }
    exitStatus = static_cast<int>(readLittleEndianUInt32(responseBytes + 2));
    Buffer stdoutBuffer;
    Buffer stderrBuffer;
    ok = readOuterctlApiStringRef(response, 6, stdoutBuffer) &&
        readOuterctlApiStringRef(response, 14, stderrBuffer);
    if (ok && stdoutBuffer.size > 0) fwrite(stdoutBuffer.data, 1, stdoutBuffer.size, stdout);
    if (ok && stderrBuffer.size > 0) fwrite(stderrBuffer.data, 1, stderrBuffer.size, stderr);
    freeBuffer(stdoutBuffer);
    freeBuffer(stderrBuffer);
    freeBuffer(response);
    if (!ok) assignCString(errorMessage, "outershelld API response payload is invalid.");
    return ok;
}

} // namespace

int main(int argc, char *argv[]) {
    int apiExitStatus = 1;
    Buffer apiError;
    CommandRequest request;
    if (!parseCommandRequest(argc, argv, request, apiError)) {
        fprintf(stderr, "%s\n", apiError.data ? apiError.data : "Invalid outerctl command.");
        freeBuffer(apiError);
        return 1;
    }
    Buffer endpointHost;
    Buffer endpointPath;
    if (request.messageType == kMessageAppAddRequest &&
        !parseAppEndpoint(request, endpointHost, endpointPath, apiError)) {
        fprintf(stderr, "%s\n", apiError.data ? apiError.data : "Invalid app endpoint.");
        freeBuffer(endpointHost);
        freeBuffer(endpointPath);
        freeBuffer(apiError);
        return 1;
    }
    if (tryOuterctlApi(request, apiExitStatus, apiError)) {
        freeBuffer(endpointHost);
        freeBuffer(endpointPath);
        freeBuffer(apiError);
        return apiExitStatus;
    }
    fprintf(stderr, "%s\n", apiError.data ? apiError.data : "Failed to call outershelld API.");
    freeBuffer(endpointHost);
    freeBuffer(endpointPath);
    freeBuffer(apiError);
    return 1;
}
