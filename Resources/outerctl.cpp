#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

extern "C" {
#if defined(__APPLE__)
#include <sqlite3.h>
#else
#include "ThirdParty/sqlite/sqlite3.h"
#endif
}

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {

enum class RegistryFieldKey : uint8_t {
    identifier = 1,
    displayName = 2,
    hostedApps = 3,
    systemdUnitName = 5,
};

struct Buffer {
    char* data;
    size_t size;
    size_t capacity;
};

struct HostedAppEntry {
    int port;
    Buffer socketPath;
    Buffer name;
    Buffer iconPath;
    Buffer url;
    Buffer list;
};

struct HostedAppArray {
    HostedAppEntry* items;
    size_t count;
    size_t capacity;
};

struct RegistryField {
    uint8_t key;
    Buffer value;
};

struct RegistryFieldArray {
    RegistryField* items;
    size_t count;
    size_t capacity;
};

struct RegistryRow {
    Buffer identifier;
    RegistryFieldArray fields;
};

struct RegistryRowArray {
    RegistryRow* items;
    size_t count;
    size_t capacity;
};

#if defined(__APPLE__)
constexpr const char* kOuterwebappsLibraryDirectoryRelativePath = "Library/Application Support/outerwebapps";
#else
constexpr const char* kOuterwebappsStateDirectoryRelativePath = ".local/state/outerwebapps";
#endif
constexpr const char* kRegistryDatabaseFileName = "registry.sqlite3";
constexpr const char* kRegistryBinaryFileName = "registry.orwa";
constexpr const char* kRegistryBinaryLockFileName = "registry.orwa.lock";
constexpr const char* kLegacyRegistryStorageFileName = "registry.bin";
constexpr const char* kHostedAppOpenedDistributedNotificationName = "dev.outergroup.outerloop.hostedAppDidOpen";
constexpr const char* kHostedAppOpenedDistributedNotificationBackendUserInfoKey = "backend";
constexpr const char* kHostedAppOpenedDistributedNotificationPortUserInfoKey = "port";
constexpr const char* kServiceUIsInvalidatedDistributedNotificationName = "dev.outergroup.outerloop.serviceUIsInvalidated";
constexpr uint32_t kOuterctlApiMaxFrameLength = 65536;

enum {
    ORWA_TABLE_BACKENDS = 0,
    ORWA_TABLE_FRONTENDS = 1,
    ORWA_TABLE_FRONTEND_LAYOUTS = 2,
    ORWA_TABLE_LOG_FILES = 3,
    ORWA_TABLE_COUNT = 4,
    ORWA_LEGACY_THREE_TABLE_COUNT = 3,
    ORWA_LEGACY_TABLE_SYSTEMD_BACKENDS = 3,
    ORWA_LEGACY_TABLE_LAUNCHD_BACKENDS = 4,
    ORWA_LEGACY_TABLE_COUNT = 5,
    ORWA_TABLE_DESCRIPTOR_SIZE = 20,
    ORWA_HEADER_SIZE = 8 + ORWA_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_THREE_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_THREE_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_FIVE_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_HEADER_SIZE = 168,
    ORWA_LEGACY_TABLE_DESCRIPTOR_SIZE = 24,
    ORWA_BACKENDS_ROW_SIZE = 84,
    ORWA_LEGACY_BACKENDS_ROW_SIZE = 64,
    ORWA_FRONTENDS_ROW_SIZE = 97,
    ORWA_LEGACY_FRONTENDS_ROW_SIZE = 104,
    ORWA_FRONTEND_LAYOUTS_ROW_SIZE = 32,
    ORWA_LOG_FILES_ROW_SIZE = 32,
    ORWA_LEGACY_SYSTEMD_BACKENDS_ROW_SIZE_CURRENT = 32,
    ORWA_LEGACY_SYSTEMD_BACKENDS_ROW_SIZE = 48,
    ORWA_LAUNCHD_BACKENDS_ROW_SIZE = 40
};

struct OrwaTableDescriptor {
    uint64_t offset = 0;
    uint64_t rowCount = 0;
    uint32_t rowSize = 0;
};

struct OrwaStringEntry {
    std::string value;
    uint64_t offset = 0;
};

struct OrwaStringPool {
    std::vector<OrwaStringEntry> entries;
    uint64_t variableBaseOffset = 0;
};

void initBuffer(Buffer& buffer) {
    buffer.data = nullptr;
    buffer.size = 0;
    buffer.capacity = 0;
}

void freeBuffer(Buffer& buffer) {
    free(buffer.data);
    buffer.data = nullptr;
    buffer.size = 0;
    buffer.capacity = 0;
}

void clearBuffer(Buffer& buffer) {
    buffer.size = 0;
    if (buffer.data) {
        buffer.data[0] = '\0';
    }
}

bool reserveBuffer(Buffer& buffer, size_t capacity) {
    if (capacity <= buffer.capacity) {
        return true;
    }

    size_t newCapacity = buffer.capacity == 0 ? 64 : buffer.capacity;
    while (newCapacity < capacity) {
        if (newCapacity > (SIZE_MAX / 2)) {
            newCapacity = capacity;
            break;
        }
        newCapacity *= 2;
    }

    char* newData = static_cast<char*>(realloc(buffer.data, newCapacity + 1));
    if (!newData) {
        return false;
    }

    buffer.data = newData;
    buffer.capacity = newCapacity;
    buffer.data[buffer.size] = '\0';
    return true;
}

bool appendBuffer(Buffer& buffer, const void* data, size_t size) {
    if (size == 0) {
        return true;
    }
    if (!reserveBuffer(buffer, buffer.size + size)) {
        return false;
    }
    memcpy(buffer.data + buffer.size, data, size);
    buffer.size += size;
    buffer.data[buffer.size] = '\0';
    return true;
}

bool appendCString(Buffer& buffer, const char* value) {
    return appendBuffer(buffer, value, strlen(value));
}

bool appendByte(Buffer& buffer, uint8_t value) {
    return appendBuffer(buffer, &value, sizeof(value));
}

bool assignBuffer(Buffer& buffer, const void* data, size_t size) {
    clearBuffer(buffer);
    return appendBuffer(buffer, data, size);
}

bool assignCString(Buffer& buffer, const char* value) {
    return assignBuffer(buffer, value, strlen(value));
}

bool ensureHostedAppURLHasPath(Buffer& buffer) {
    const char* value = buffer.data ? buffer.data : "";
    const char* schemeSeparator = strstr(value, "://");
    const char* authorityStart = schemeSeparator ? schemeSeparator + 3 : value;
    const char* slash = strchr(authorityStart, '/');
    const char* query = strchr(authorityStart, '?');
    const char* fragment = strchr(authorityStart, '#');
    const char* firstSuffix = query;
    if (!firstSuffix || (fragment && fragment < firstSuffix)) {
        firstSuffix = fragment;
    }

    if (slash && (!firstSuffix || slash < firstSuffix)) {
        return true;
    }
    if (!firstSuffix) {
        return appendByte(buffer, '/');
    }

    Buffer normalized;
    initBuffer(normalized);
    const size_t prefixLength = static_cast<size_t>(firstSuffix - value);
    const bool ok = appendBuffer(normalized, value, prefixLength) &&
        appendByte(normalized, '/') &&
        appendCString(normalized, firstSuffix);
    if (ok) {
        freeBuffer(buffer);
        buffer = normalized;
        initBuffer(normalized);
    }
    freeBuffer(normalized);
    return ok;
}

bool assignHostedAppURL(Buffer& buffer, const char* value, int port, const char* socketPath) {
    clearBuffer(buffer);

    const char* safeSocketPath = socketPath ? socketPath : "";
    if (safeSocketPath[0] != '\0') {
        const char* safeValue = value ? value : "";
        if (safeValue[0] == '\0') {
            return appendCString(buffer, safeSocketPath) &&
                appendByte(buffer, '/');
        }
        if (strcmp(safeValue, safeSocketPath) == 0) {
            return appendCString(buffer, safeSocketPath) &&
                appendByte(buffer, '/');
        }
        if (strncmp(safeValue, safeSocketPath, strlen(safeSocketPath)) == 0) {
            return assignCString(buffer, safeValue);
        }
        if (!appendCString(buffer, safeSocketPath)) {
            return false;
        }
        if (safeValue[0] != '/' && safeValue[0] != '?' && safeValue[0] != '#') {
            if (!appendByte(buffer, '/')) {
                return false;
            }
        } else if (safeValue[0] == '?' || safeValue[0] == '#') {
            if (!appendByte(buffer, '/')) {
                return false;
            }
        }
        return appendCString(buffer, safeValue);
    }

    char portBuffer[32];
    snprintf(portBuffer, sizeof(portBuffer), "%d", port < 0 ? 0 : port);

    const char* safeValue = value ? value : "";
    const char* schemeSeparator = strstr(safeValue, "://");
    if (safeValue[0] == '\0') {
        if (!appendCString(buffer, "http://localhost:") ||
            !appendCString(buffer, portBuffer)) {
            return false;
        }
        return appendByte(buffer, '/');
    }

    if (schemeSeparator) {
        if (!assignCString(buffer, safeValue)) {
            return false;
        }
        return ensureHostedAppURLHasPath(buffer);
    }

    if (safeValue[0] == '/' || safeValue[0] == '?' || safeValue[0] == '#') {
        if (!appendCString(buffer, "http://localhost:") ||
            !appendCString(buffer, portBuffer)) {
            return false;
        }
        if (safeValue[0] != '/' && !appendByte(buffer, '/')) {
            return false;
        }
        if (!appendCString(buffer, safeValue)) {
            return false;
        }
        return ensureHostedAppURLHasPath(buffer);
    }

    if (!appendCString(buffer, "http://") ||
        !appendCString(buffer, safeValue)) {
        return false;
    }
    return ensureHostedAppURLHasPath(buffer);
}

bool appendPathComponent(Buffer& path, const char* component) {
    if (!component || component[0] == '\0') {
        return true;
    }
    if (path.size != 0 && path.data[path.size - 1] != '/') {
        if (!appendByte(path, '/')) {
            return false;
        }
    }
    return appendCString(path, component);
}

bool buffersEqual(const Buffer& left, const Buffer& right) {
    return left.size == right.size && memcmp(left.data, right.data, left.size) == 0;
}

void moveBuffer(Buffer& destination, Buffer& source) {
    freeBuffer(destination);
    destination = source;
    initBuffer(source);
}

bool setError(Buffer& errorMessage, const char* message) {
    return assignCString(errorMessage, message);
}

bool setErrnoError(Buffer& errorMessage, const char* prefix) {
    clearBuffer(errorMessage);
    if (!appendCString(errorMessage, prefix) ||
        !appendCString(errorMessage, strerror(errno))) {
        clearBuffer(errorMessage);
        return false;
    }
    return true;
}

uint16_t readLittleEndianUInt16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
        (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readLittleEndianUInt32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t readLittleEndianUInt64(const uint8_t* bytes) {
    return static_cast<uint64_t>(bytes[0]) |
        (static_cast<uint64_t>(bytes[1]) << 8) |
        (static_cast<uint64_t>(bytes[2]) << 16) |
        (static_cast<uint64_t>(bytes[3]) << 24) |
        (static_cast<uint64_t>(bytes[4]) << 32) |
        (static_cast<uint64_t>(bytes[5]) << 40) |
        (static_cast<uint64_t>(bytes[6]) << 48) |
        (static_cast<uint64_t>(bytes[7]) << 56);
}

void writeLittleEndianUInt32(uint8_t* bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value & 0xff);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    bytes[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void writeLittleEndianUInt16(uint8_t* bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value & 0xff);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void writeLittleEndianUInt64(uint8_t* bytes, uint64_t value) {
    bytes[0] = static_cast<uint8_t>(value & 0xff);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    bytes[3] = static_cast<uint8_t>((value >> 24) & 0xff);
    bytes[4] = static_cast<uint8_t>((value >> 32) & 0xff);
    bytes[5] = static_cast<uint8_t>((value >> 40) & 0xff);
    bytes[6] = static_cast<uint8_t>((value >> 48) & 0xff);
    bytes[7] = static_cast<uint8_t>((value >> 56) & 0xff);
}

bool appendLittleEndianUInt32(Buffer& buffer, uint32_t value) {
    uint8_t bytes[4];
    writeLittleEndianUInt32(bytes, value);
    return appendBuffer(buffer, bytes, sizeof(bytes));
}

bool appendLittleEndianUInt16(Buffer& buffer, uint16_t value) {
    uint8_t bytes[2];
    writeLittleEndianUInt16(bytes, value);
    return appendBuffer(buffer, bytes, sizeof(bytes));
}

bool appendLittleEndianUInt64(Buffer& buffer, uint64_t value) {
    uint8_t bytes[8];
    writeLittleEndianUInt64(bytes, value);
    return appendBuffer(buffer, bytes, sizeof(bytes));
}

bool appendZeroBytes(Buffer& buffer, size_t size) {
    if (!reserveBuffer(buffer, buffer.size + size)) {
        return false;
    }
    memset(buffer.data + buffer.size, 0, size);
    buffer.size += size;
    buffer.data[buffer.size] = '\0';
    return true;
}

bool buildOuterwebappsPath(const char* fileName, Buffer& path);

bool writeBufferLittleEndianUInt32At(Buffer& buffer, size_t offset, uint32_t value) {
    if (offset + 4 > buffer.size) {
        return false;
    }
    writeLittleEndianUInt32(reinterpret_cast<uint8_t*>(buffer.data + offset), value);
    return true;
}

bool appendOuterctlApiStringRef(Buffer& message, size_t refOffset, const char* value) {
    const char* safeValue = value ? value : "";
    const size_t length = strlen(safeValue);
    if (message.size > UINT32_MAX || length > UINT32_MAX) {
        return false;
    }
    const uint32_t offset = static_cast<uint32_t>(message.size);
    return appendBuffer(message, safeValue, length) &&
        writeBufferLittleEndianUInt32At(message, refOffset, offset) &&
        writeBufferLittleEndianUInt32At(message, refOffset + 4, static_cast<uint32_t>(length));
}

bool readExact(int fd, void* data, size_t size) {
    char* bytes = static_cast<char*>(data);
    while (size > 0) {
        ssize_t got = read(fd, bytes, size);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            return false;
        }
        bytes += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

bool writeExact(int fd, const void* data, size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t wrote = write(fd, bytes, size);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += wrote;
        size -= static_cast<size_t>(wrote);
    }
    return true;
}

void defaultOuterctlApiSocketPath(char* out, size_t outSize) {
    const char* envPath = getenv("OUTERSHELLD_API_SOCKET");
    if (envPath && envPath[0]) {
        snprintf(out, outSize, "%s", envPath);
        return;
    }
#if defined(__APPLE__)
    const char* tmp = getenv("DARWIN_USER_TEMP_DIR");
    if (!tmp || !tmp[0]) {
        tmp = getenv("TMPDIR");
    }
    if (tmp && tmp[0]) {
        snprintf(out, outSize, "%s%soutershelld-api", tmp, tmp[strlen(tmp) - 1] == '/' ? "" : "/");
        return;
    }
    snprintf(out, outSize, "/tmp/outershelld-api-%d", static_cast<int>(getuid()));
#else
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(out, outSize, "%s/outershelld-api", runtime);
        return;
    }
    snprintf(out, outSize, "/run/user/%d/outershelld-api", static_cast<int>(getuid()));
#endif
}

bool readOuterctlApiStringRef(const Buffer& message, size_t refOffset, Buffer& out) {
    if (refOffset + 8 > message.size) {
        return false;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(message.data);
    const uint32_t offset = readLittleEndianUInt32(bytes + refOffset);
    const uint32_t length = readLittleEndianUInt32(bytes + refOffset + 4);
    if (offset > message.size || length > message.size - offset) {
        return false;
    }
    return assignBuffer(out, message.data + offset, length);
}

bool outerctlApiRegistryPath(Buffer& path) {
    clearBuffer(path);
    const char* registry = getenv("OUTERWEBAPPS_REGISTRY");
    if (!registry || !registry[0]) {
        registry = getenv("BACKENDS_REGISTRY_DB");
    }
    if (registry && registry[0]) {
        return appendCString(path, registry);
    }
    return buildOuterwebappsPath(kRegistryDatabaseFileName, path);
}

bool tryOuterctlApi(int argc, char* argv[], int& exitStatus, Buffer& errorMessage) {
    char socketPath[PATH_MAX];
    defaultOuterctlApiSocketPath(socketPath, sizeof(socketPath));
    if (!socketPath[0]) {
        assignCString(errorMessage, "Could not resolve outershelld API socket path.");
        return false;
    }

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
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        clearBuffer(errorMessage);
        appendCString(errorMessage, "Failed to connect to outershelld API socket ");
        appendCString(errorMessage, socketPath);
        appendCString(errorMessage, ": ");
        appendCString(errorMessage, strerror(errno));
        close(fd);
        return false;
    }

    Buffer message;
    initBuffer(message);
    Buffer registryPath;
    initBuffer(registryPath);
    bool ok = appendLittleEndianUInt16(message, 1) &&
        appendLittleEndianUInt32(message, static_cast<uint32_t>(argc)) &&
        outerctlApiRegistryPath(registryPath);
    const size_t registryRefOffset = message.size;
    const size_t refsOffset = registryRefOffset + 8;
    ok = ok && appendZeroBytes(message, 8 + static_cast<size_t>(argc) * 8);
    ok = ok && appendOuterctlApiStringRef(message, registryRefOffset, registryPath.data ? registryPath.data : "");
    for (int i = 0; ok && i < argc; i += 1) {
        ok = appendOuterctlApiStringRef(message, refsOffset + static_cast<size_t>(i) * 8, argv[i]);
    }
    if (!ok || message.size > UINT32_MAX) {
        freeBuffer(registryPath);
        freeBuffer(message);
        close(fd);
        assignCString(errorMessage, "Failed to encode outershelld API request.");
        return false;
    }
    freeBuffer(registryPath);

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
    if (responseLength < 22 || responseLength > kOuterctlApiMaxFrameLength) {
        close(fd);
        assignCString(errorMessage, "outershelld API response frame is invalid.");
        return false;
    }
    Buffer response;
    initBuffer(response);
    ok = reserveBuffer(response, responseLength) &&
        readExact(fd, response.data, responseLength);
    close(fd);
    if (!ok) {
        freeBuffer(response);
        setErrnoError(errorMessage, "Failed to read outershelld API response: ");
        return false;
    }
    response.size = responseLength;
    response.data[response.size] = '\0';

    const uint8_t* responseBytes = reinterpret_cast<const uint8_t*>(response.data);
    if (readLittleEndianUInt16(responseBytes) != 2) {
        freeBuffer(response);
        assignCString(errorMessage, "outershelld API response type is invalid.");
        return false;
    }
    exitStatus = static_cast<int>(readLittleEndianUInt32(responseBytes + 2));
    Buffer stdoutBuffer;
    Buffer stderrBuffer;
    initBuffer(stdoutBuffer);
    initBuffer(stderrBuffer);
    ok = readOuterctlApiStringRef(response, 6, stdoutBuffer) &&
        readOuterctlApiStringRef(response, 14, stderrBuffer);
    if (ok && stdoutBuffer.size > 0) {
        fwrite(stdoutBuffer.data, 1, stdoutBuffer.size, stdout);
    }
    if (ok && stderrBuffer.size > 0) {
        fwrite(stderrBuffer.data, 1, stderrBuffer.size, stderr);
    }
    freeBuffer(stdoutBuffer);
    freeBuffer(stderrBuffer);
    freeBuffer(response);
    if (!ok) {
        assignCString(errorMessage, "outershelld API response payload is invalid.");
    }
    return ok;
}

void initHostedAppEntry(HostedAppEntry& entry) {
    entry.port = 0;
    initBuffer(entry.socketPath);
    initBuffer(entry.name);
    initBuffer(entry.iconPath);
    initBuffer(entry.url);
    initBuffer(entry.list);
}

void freeHostedAppEntry(HostedAppEntry& entry) {
    freeBuffer(entry.socketPath);
    freeBuffer(entry.name);
    freeBuffer(entry.iconPath);
    freeBuffer(entry.url);
    freeBuffer(entry.list);
    entry.port = 0;
}

void initHostedAppArray(HostedAppArray& array) {
    array.items = nullptr;
    array.count = 0;
    array.capacity = 0;
}

void freeHostedAppArray(HostedAppArray& array) {
    for (size_t i = 0; i < array.count; i += 1) {
        freeHostedAppEntry(array.items[i]);
    }
    free(array.items);
    array.items = nullptr;
    array.count = 0;
    array.capacity = 0;
}

bool reserveHostedAppArray(HostedAppArray& array, size_t capacity) {
    if (capacity <= array.capacity) {
        return true;
    }

    size_t newCapacity = array.capacity == 0 ? 4 : array.capacity;
    while (newCapacity < capacity) {
        if (newCapacity > (SIZE_MAX / 2)) {
            newCapacity = capacity;
            break;
        }
        newCapacity *= 2;
    }

    HostedAppEntry* newItems = static_cast<HostedAppEntry*>(realloc(array.items, newCapacity * sizeof(HostedAppEntry)));
    if (!newItems) {
        return false;
    }

    array.items = newItems;
    array.capacity = newCapacity;
    return true;
}

bool appendHostedAppMove(HostedAppArray& array, HostedAppEntry& entry) {
    if (!reserveHostedAppArray(array, array.count + 1)) {
        return false;
    }
    array.items[array.count] = entry;
    array.count += 1;
    initHostedAppEntry(entry);
    return true;
}

bool hostedAppMatches(const HostedAppEntry& entry, int port, const char* socketPath) {
    const char* safeSocketPath = socketPath ? socketPath : "";
    if (safeSocketPath[0] != '\0') {
        return entry.socketPath.data &&
            strcmp(entry.socketPath.data, safeSocketPath) == 0;
    }
    return entry.port == port;
}

void removeHostedAppsMatching(HostedAppArray& array, int port, const char* socketPath) {
    size_t writeIndex = 0;
    for (size_t readIndex = 0; readIndex < array.count; readIndex += 1) {
        if (hostedAppMatches(array.items[readIndex], port, socketPath)) {
            freeHostedAppEntry(array.items[readIndex]);
            continue;
        }
        if (writeIndex != readIndex) {
            array.items[writeIndex] = array.items[readIndex];
            initHostedAppEntry(array.items[readIndex]);
        }
        writeIndex += 1;
    }
    array.count = writeIndex;
}

void initRegistryField(RegistryField& field) {
    field.key = 0;
    initBuffer(field.value);
}

void freeRegistryField(RegistryField& field) {
    freeBuffer(field.value);
    field.key = 0;
}

void initRegistryFieldArray(RegistryFieldArray& array) {
    array.items = nullptr;
    array.count = 0;
    array.capacity = 0;
}

void freeRegistryFieldArray(RegistryFieldArray& array) {
    for (size_t i = 0; i < array.count; i += 1) {
        freeRegistryField(array.items[i]);
    }
    free(array.items);
    array.items = nullptr;
    array.count = 0;
    array.capacity = 0;
}

bool reserveRegistryFieldArray(RegistryFieldArray& array, size_t capacity) {
    if (capacity <= array.capacity) {
        return true;
    }

    size_t newCapacity = array.capacity == 0 ? 4 : array.capacity;
    while (newCapacity < capacity) {
        if (newCapacity > (SIZE_MAX / 2)) {
            newCapacity = capacity;
            break;
        }
        newCapacity *= 2;
    }

    RegistryField* newItems = static_cast<RegistryField*>(realloc(array.items, newCapacity * sizeof(RegistryField)));
    if (!newItems) {
        return false;
    }

    array.items = newItems;
    array.capacity = newCapacity;
    return true;
}

ssize_t findFieldIndex(const RegistryFieldArray& fields, uint8_t key) {
    for (size_t i = 0; i < fields.count; i += 1) {
        if (fields.items[i].key == key) {
            return static_cast<ssize_t>(i);
        }
    }
    return -1;
}

Buffer* findFieldValue(RegistryFieldArray& fields, uint8_t key) {
    const ssize_t index = findFieldIndex(fields, key);
    if (index < 0) {
        return nullptr;
    }
    return &fields.items[index].value;
}

bool setFieldValue(RegistryFieldArray& fields, uint8_t key, const void* data, size_t size) {
    const ssize_t existingIndex = findFieldIndex(fields, key);
    if (existingIndex >= 0) {
        return assignBuffer(fields.items[existingIndex].value, data, size);
    }

    if (!reserveRegistryFieldArray(fields, fields.count + 1)) {
        return false;
    }

    RegistryField& field = fields.items[fields.count];
    initRegistryField(field);
    field.key = key;
    if (!assignBuffer(field.value, data, size)) {
        return false;
    }
    fields.count += 1;
    return true;
}

void moveRegistryFieldArray(RegistryFieldArray& destination, RegistryFieldArray& source) {
    freeRegistryFieldArray(destination);
    destination = source;
    initRegistryFieldArray(source);
}

void initRegistryRow(RegistryRow& row) {
    initBuffer(row.identifier);
    initRegistryFieldArray(row.fields);
}

void freeRegistryRow(RegistryRow& row) {
    freeBuffer(row.identifier);
    freeRegistryFieldArray(row.fields);
}

void initRegistryRowArray(RegistryRowArray& array) {
    array.items = nullptr;
    array.count = 0;
    array.capacity = 0;
}

void freeRegistryRowArray(RegistryRowArray& array) {
    for (size_t i = 0; i < array.count; i += 1) {
        freeRegistryRow(array.items[i]);
    }
    free(array.items);
    array.items = nullptr;
    array.count = 0;
    array.capacity = 0;
}

bool reserveRegistryRowArray(RegistryRowArray& array, size_t capacity) {
    if (capacity <= array.capacity) {
        return true;
    }

    size_t newCapacity = array.capacity == 0 ? 4 : array.capacity;
    while (newCapacity < capacity) {
        if (newCapacity > (SIZE_MAX / 2)) {
            newCapacity = capacity;
            break;
        }
        newCapacity *= 2;
    }

    RegistryRow* newItems = static_cast<RegistryRow*>(realloc(array.items, newCapacity * sizeof(RegistryRow)));
    if (!newItems) {
        return false;
    }

    array.items = newItems;
    array.capacity = newCapacity;
    return true;
}

ssize_t findRowIndex(const RegistryRowArray& rows, const Buffer& identifier) {
    for (size_t i = 0; i < rows.count; i += 1) {
        if (buffersEqual(rows.items[i].identifier, identifier)) {
            return static_cast<ssize_t>(i);
        }
    }
    return -1;
}

bool upsertLoadedRow(RegistryRowArray& rows, Buffer& identifier, RegistryFieldArray& fields) {
    const ssize_t existingIndex = findRowIndex(rows, identifier);
    if (existingIndex >= 0) {
        RegistryRow& row = rows.items[existingIndex];
        moveBuffer(row.identifier, identifier);
        moveRegistryFieldArray(row.fields, fields);
        return true;
    }

    if (!reserveRegistryRowArray(rows, rows.count + 1)) {
        return false;
    }

    RegistryRow& row = rows.items[rows.count];
    initRegistryRow(row);
    moveBuffer(row.identifier, identifier);
    moveRegistryFieldArray(row.fields, fields);
    rows.count += 1;
    return true;
}

bool resolveOuterwebappsHome(Buffer& path) {
    clearBuffer(path);

    const char* overrideRoot = getenv("OUTERWEBAPPS_HOME");
    if (overrideRoot && overrideRoot[0] != '\0') {
        return appendCString(path, overrideRoot);
    }

#if !defined(__APPLE__)
    const char* stateHome = getenv("XDG_STATE_HOME");
    if (stateHome && stateHome[0] != '\0') {
        return appendCString(path, stateHome) &&
               appendPathComponent(path, "outerwebapps");
    }
#endif

    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
#ifdef __APPLE__
        return appendCString(path, home) &&
               appendPathComponent(path, kOuterwebappsLibraryDirectoryRelativePath);
#else
        return appendCString(path, home) &&
               appendPathComponent(path, kOuterwebappsStateDirectoryRelativePath);
#endif
    }

    const passwd* entry = getpwuid(geteuid());
    if (entry && entry->pw_dir && entry->pw_dir[0] != '\0') {
#ifdef __APPLE__
        return appendCString(path, entry->pw_dir) &&
               appendPathComponent(path, kOuterwebappsLibraryDirectoryRelativePath);
#else
        return appendCString(path, entry->pw_dir) &&
               appendPathComponent(path, kOuterwebappsStateDirectoryRelativePath);
#endif
    }

#ifdef __APPLE__
    return appendCString(path, kOuterwebappsLibraryDirectoryRelativePath);
#else
    return appendCString(path, kOuterwebappsStateDirectoryRelativePath);
#endif
}

bool buildOuterwebappsPath(const char* fileName, Buffer& path) {
    return resolveOuterwebappsHome(path) && appendPathComponent(path, fileName);
}

bool ensureDirectoryExists(const char* path, Buffer& errorMessage) {
    if (!path || path[0] == '\0') {
        return true;
    }

    char* mutablePath = static_cast<char*>(malloc(strlen(path) + 1));
    if (!mutablePath) {
        return setError(errorMessage, "Out of memory.");
    }
    strcpy(mutablePath, path);

    const size_t length = strlen(mutablePath);
    for (size_t i = 1; i <= length; i += 1) {
        if (mutablePath[i] != '/' && mutablePath[i] != '\0') {
            continue;
        }
        const char saved = mutablePath[i];
        mutablePath[i] = '\0';
        if (mutablePath[0] != '\0') {
            if (mkdir(mutablePath, 0700) != 0 && errno != EEXIST) {
                setErrnoError(errorMessage, "Failed to create outerwebapps home: ");
                free(mutablePath);
                return false;
            }
        }
        mutablePath[i] = saved;
    }

    free(mutablePath);
    return true;
}

class RegistryFileLock {
public:
    ~RegistryFileLock() {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
        }
    }

    bool acquire(Buffer& errorMessage) {
        Buffer rootPath;
        Buffer lockPath;
        initBuffer(rootPath);
        initBuffer(lockPath);

        const bool haveRootPath = resolveOuterwebappsHome(rootPath);
        const bool directoryReady = haveRootPath && ensureDirectoryExists(rootPath.data, errorMessage);
        const bool haveLockPath = directoryReady && buildOuterwebappsPath(kRegistryBinaryLockFileName, lockPath);
        if (!haveRootPath || !haveLockPath) {
            if (errorMessage.size == 0) {
                setError(errorMessage, "Out of memory.");
            }
            freeBuffer(rootPath);
            freeBuffer(lockPath);
            return false;
        }

        const int fd = open(lockPath.data, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            setErrnoError(errorMessage, "Failed to open registry lock: ");
            freeBuffer(rootPath);
            freeBuffer(lockPath);
            return false;
        }
        (void)fchmod(fd, 0666);
        if (flock(fd, LOCK_EX) != 0) {
            setErrnoError(errorMessage, "Failed to lock registry: ");
            close(fd);
            freeBuffer(rootPath);
            freeBuffer(lockPath);
            return false;
        }

        fd_ = fd;
        freeBuffer(rootPath);
        freeBuffer(lockPath);
        return true;
    }

private:
    int fd_ = -1;
};

bool readBinaryField(const Buffer& input, size_t& cursor, Buffer& value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(input.data);
    if (cursor + sizeof(uint16_t) > input.size) {
        return false;
    }
    const uint16_t length = readLittleEndianUInt16(bytes + cursor);
    cursor += sizeof(uint16_t);
    if (cursor + length > input.size) {
        return false;
    }
    const bool assigned = assignBuffer(value, input.data + cursor, length);
    cursor += length;
    return assigned;
}

bool deserializeHostedApps(const Buffer& input, HostedAppArray& apps, Buffer& errorMessage) {
    freeHostedAppArray(apps);
    initHostedAppArray(apps);

    if (input.size == 0) {
        return true;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(input.data);
    size_t cursor = 0;
    if (cursor + sizeof(uint16_t) > input.size) {
        return setError(errorMessage, "Hosted apps payload is truncated (count).");
    }
    const uint16_t count = readLittleEndianUInt16(bytes + cursor);
    cursor += sizeof(uint16_t);

    if (!reserveHostedAppArray(apps, count)) {
        return setError(errorMessage, "Out of memory.");
    }

    for (uint16_t i = 0; i < count; i += 1) {
        if (cursor + sizeof(uint16_t) > input.size) {
            return setError(errorMessage, "Hosted apps payload is truncated (port).");
        }

        HostedAppEntry entry;
        initHostedAppEntry(entry);
        entry.port = readLittleEndianUInt16(bytes + cursor);
        cursor += sizeof(uint16_t);

        if (!readBinaryField(input, cursor, entry.name) ||
            !readBinaryField(input, cursor, entry.iconPath) ||
            !readBinaryField(input, cursor, entry.url)) {
            freeHostedAppEntry(entry);
            return setError(errorMessage, "Hosted apps payload is truncated (field).");
        }

        if (!appendHostedAppMove(apps, entry)) {
            freeHostedAppEntry(entry);
            return setError(errorMessage, "Out of memory.");
        }
    }

    if (cursor != input.size) {
        return setError(errorMessage, "Hosted apps payload contains trailing bytes.");
    }

    return true;
}

bool loadFile(Buffer& path, Buffer& contents, Buffer& errorMessage) {
    clearBuffer(contents);

    FILE* file = fopen(path.data, "rb");
    if (!file) {
        if (errno == ENOENT) {
            return true;
        }
        return setError(errorMessage, "Failed to open registry storage.");
    }

    char chunk[4096];
    while (true) {
        const size_t count = fread(chunk, 1, sizeof(chunk), file);
        if (count > 0 && !appendBuffer(contents, chunk, count)) {
            fclose(file);
            return setError(errorMessage, "Out of memory.");
        }
        if (count < sizeof(chunk)) {
            if (feof(file) != 0) {
                break;
            }
            if (ferror(file) != 0) {
                fclose(file);
                clearBuffer(contents);
                return setError(errorMessage, "Failed to read registry storage.");
            }
        }
    }

    fclose(file);
    return true;
}

bool loadLegacyRegistryRows(RegistryRowArray& rows, Buffer& errorMessage) {
    freeRegistryRowArray(rows);
    initRegistryRowArray(rows);

    Buffer path;
    Buffer contents;
    initBuffer(path);
    initBuffer(contents);

    if (!buildOuterwebappsPath(kLegacyRegistryStorageFileName, path)) {
        freeBuffer(path);
        freeBuffer(contents);
        return setError(errorMessage, "Out of memory.");
    }

    if (!loadFile(path, contents, errorMessage)) {
        freeBuffer(path);
        freeBuffer(contents);
        return false;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(contents.data);
    size_t cursor = 0;
    while (cursor < contents.size) {
        if (cursor + sizeof(uint32_t) > contents.size) {
            freeBuffer(path);
            freeBuffer(contents);
            return setError(errorMessage, "Registry storage is truncated (row length).");
        }

        const uint32_t rowLength = readLittleEndianUInt32(bytes + cursor);
        cursor += sizeof(uint32_t);
        if (cursor + rowLength > contents.size) {
            freeBuffer(path);
            freeBuffer(contents);
            return setError(errorMessage, "Registry storage is truncated (row payload).");
        }

        size_t rowCursor = cursor;
        const size_t rowEnd = cursor + rowLength;
        if (rowCursor + sizeof(uint16_t) > rowEnd) {
            freeBuffer(path);
            freeBuffer(contents);
            return setError(errorMessage, "Registry row is truncated (pair count).");
        }

        const uint16_t pairCount = readLittleEndianUInt16(bytes + rowCursor);
        rowCursor += sizeof(uint16_t);

        RegistryFieldArray fields;
        initRegistryFieldArray(fields);

        bool parseSucceeded = true;
        for (uint16_t i = 0; i < pairCount; i += 1) {
            if (rowCursor + 3 > rowEnd) {
                parseSucceeded = setError(errorMessage, "Registry row is truncated (field header).");
                break;
            }

            const uint8_t key = bytes[rowCursor];
            rowCursor += 1;
            const uint16_t valueLength = readLittleEndianUInt16(bytes + rowCursor);
            rowCursor += sizeof(uint16_t);
            if (rowCursor + valueLength > rowEnd) {
                parseSucceeded = setError(errorMessage, "Registry row is truncated (field value).");
                break;
            }

            if (!setFieldValue(fields, key, contents.data + rowCursor, valueLength)) {
                parseSucceeded = setError(errorMessage, "Out of memory.");
                break;
            }
            rowCursor += valueLength;
        }

        if (!parseSucceeded) {
            freeRegistryFieldArray(fields);
            freeBuffer(path);
            freeBuffer(contents);
            return false;
        }

        Buffer* identifierValue = findFieldValue(fields, static_cast<uint8_t>(RegistryFieldKey::identifier));
        if (identifierValue && identifierValue->size != 0) {
            Buffer identifier;
            initBuffer(identifier);
            if (!assignBuffer(identifier, identifierValue->data, identifierValue->size)) {
                freeRegistryFieldArray(fields);
                freeBuffer(identifier);
                freeBuffer(path);
                freeBuffer(contents);
                return setError(errorMessage, "Out of memory.");
            }

            if (!upsertLoadedRow(rows, identifier, fields)) {
                freeRegistryFieldArray(fields);
                freeBuffer(identifier);
                freeBuffer(path);
                freeBuffer(contents);
                return setError(errorMessage, "Out of memory.");
            }
        } else {
            freeRegistryFieldArray(fields);
        }

        cursor = rowEnd;
    }

    freeBuffer(path);
    freeBuffer(contents);
    return true;
}

bool setSqliteError(Buffer& errorMessage, sqlite3* database, const char* prefix) {
    clearBuffer(errorMessage);
    return appendCString(errorMessage, prefix) &&
        appendCString(errorMessage, sqlite3_errmsg(database));
}

bool sqliteExecWithError(sqlite3* database, const char* sql, Buffer& errorMessage) {
    clearBuffer(errorMessage);
    char* rawError = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &rawError);
    if (result == SQLITE_OK) {
        return true;
    }
    if (rawError) {
        const bool assigned = assignCString(errorMessage, rawError);
        sqlite3_free(rawError);
        return assigned;
    }
    return setSqliteError(errorMessage, database, "SQLite error: ");
}

bool sqlitePrepareWithError(sqlite3* database,
                            const char* sql,
                            sqlite3_stmt*& statement,
                            Buffer& errorMessage) {
    clearBuffer(errorMessage);
    statement = nullptr;
    const int result = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        return true;
    }
    return setSqliteError(errorMessage, database, "Failed to prepare SQLite statement: ");
}

bool sqliteStepDone(sqlite3_stmt* statement, Buffer& errorMessage) {
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) {
        return true;
    }
    return setSqliteError(errorMessage, sqlite3_db_handle(statement), "SQLite statement failed: ");
}

bool frontendsTableUsesLegacySchema(sqlite3* database,
                                    bool& usesLegacySchema,
                                    Buffer& errorMessage) {
    usesLegacySchema = false;

    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "PRAGMA table_info(frontends);",
                                statement,
                                errorMessage)) {
        return false;
    }

    while (true) {
        const int stepResult = sqlite3_step(statement);
        if (stepResult == SQLITE_DONE) {
            break;
        }
        if (stepResult != SQLITE_ROW) {
            sqlite3_finalize(statement);
            return setSqliteError(errorMessage, database, "Failed to inspect frontend registry schema: ");
        }

        const char* columnName = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (columnName && strcmp(columnName, "frontend_id") == 0) {
            usesLegacySchema = true;
            break;
        }
    }

    sqlite3_finalize(statement);
    return true;
}

bool registryTableHasColumn(sqlite3* database,
                            const char* tableName,
                            const char* expectedColumn,
                            bool& hasColumn,
                            Buffer& errorMessage) {
    hasColumn = false;

    Buffer pragma;
    initBuffer(pragma);
    if (!appendCString(pragma, "PRAGMA table_info(") ||
        !appendCString(pragma, tableName) ||
        !appendCString(pragma, ");")) {
        freeBuffer(pragma);
        return setError(errorMessage, "Out of memory.");
    }

    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database, pragma.data, statement, errorMessage)) {
        freeBuffer(pragma);
        return false;
    }
    freeBuffer(pragma);

    while (true) {
        const int stepResult = sqlite3_step(statement);
        if (stepResult == SQLITE_DONE) {
            break;
        }
        if (stepResult != SQLITE_ROW) {
            sqlite3_finalize(statement);
            return setSqliteError(errorMessage, database, "Failed to inspect frontend registry columns: ");
        }

        const char* columnName = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (columnName && strcmp(columnName, expectedColumn) == 0) {
            hasColumn = true;
            break;
        }
    }

    sqlite3_finalize(statement);
    return true;
}

bool frontendsTableHasColumn(sqlite3* database,
                             const char* expectedColumn,
                             bool& hasColumn,
                             Buffer& errorMessage) {
    return registryTableHasColumn(database, "frontends", expectedColumn, hasColumn, errorMessage);
}

bool registrySupportsLaunchdBackends() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

bool registrySupportsSystemdBackends() {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

bool sqliteTableExists(sqlite3* database, const char* tableName, bool& exists, Buffer& errorMessage);

bool ensurePlatformRegistrySchema(sqlite3* database, Buffer& errorMessage) {
    if (registrySupportsLaunchdBackends()) {
        return sqliteExecWithError(database,
                                   "DROP TABLE IF EXISTS systemd_backends;",
                                   errorMessage) &&
            sqliteExecWithError(database,
                                "CREATE TABLE IF NOT EXISTS launchd_backends ("
                                "service_id TEXT PRIMARY KEY,"
                                "plist_path TEXT NOT NULL,"
                                "owns_plist INTEGER NOT NULL DEFAULT 0"
                                ");",
                                errorMessage) &&
            sqliteExecWithError(database,
                                "UPDATE backends "
                                "SET unit_name = service_id, "
                                "unit_path = (SELECT plist_path FROM launchd_backends WHERE launchd_backends.service_id = backends.service_id), "
                                "owns_unit = COALESCE((SELECT owns_plist FROM launchd_backends WHERE launchd_backends.service_id = backends.service_id), 0) "
                                "WHERE EXISTS (SELECT 1 FROM launchd_backends WHERE launchd_backends.service_id = backends.service_id) "
                                "AND (unit_name = '' OR unit_path = '');",
                                errorMessage);
    }

    if (registrySupportsSystemdBackends()) {
        return sqliteExecWithError(database,
                                   "DROP TABLE IF EXISTS launchd_backends;",
                                   errorMessage) &&
            sqliteExecWithError(database,
                                "CREATE TABLE IF NOT EXISTS systemd_backends ("
                                "service_id TEXT PRIMARY KEY,"
                                "unit_name TEXT NOT NULL"
                                ");",
                                errorMessage) &&
            sqliteExecWithError(database,
                                "INSERT INTO systemd_backends(service_id, unit_name) "
                                "SELECT service_id, service_unit "
                                "FROM backends "
                                "WHERE service_unit IS NOT NULL AND service_unit != '' "
                                "AND NOT EXISTS ("
                                "    SELECT 1 FROM systemd_backends "
                                "    WHERE systemd_backends.service_id = backends.service_id"
                                ");",
                                errorMessage) &&
            sqliteExecWithError(database,
                                "UPDATE backends "
                                "SET service_unit = NULL "
                                "WHERE service_unit IS NOT NULL AND service_unit != '';",
                                errorMessage) &&
            sqliteExecWithError(database,
                                "UPDATE backends "
                                "SET unit_name = (SELECT unit_name FROM systemd_backends WHERE systemd_backends.service_id = backends.service_id), "
                                "unit_path = '', "
                                "owns_unit = 1 "
                                "WHERE EXISTS (SELECT 1 FROM systemd_backends WHERE systemd_backends.service_id = backends.service_id) "
                                "AND unit_name = '';",
                                errorMessage);
    }

    return sqliteExecWithError(database,
                               "DROP TABLE IF EXISTS launchd_backends;",
                               errorMessage) &&
        sqliteExecWithError(database,
                            "DROP TABLE IF EXISTS systemd_backends;",
                            errorMessage);
}

bool ensureRegistrySchema(sqlite3* database, Buffer& errorMessage) {
    bool usesLegacyFrontendsTable = false;
    bool hasBackendIconPathColumn = false;
    bool hasBackendUnitNameColumn = false;
    bool hasBackendUnitPathColumn = false;
    bool hasBackendOwnsUnitColumn = false;
    bool hasFrontendDisplayNameColumn = false;
    bool hasFrontendNameColumn = false;
    bool hasFrontendIconPathColumn = false;
    bool hasSocketPathColumn = false;
    bool hasListColumn = false;
    bool hadFrontendLayoutsTable = false;
    if (!sqliteTableExists(database, "frontend_layouts", hadFrontendLayoutsTable, errorMessage)) {
        return false;
    }
    const bool schemaReady = sqliteExecWithError(database, "PRAGMA busy_timeout = 5000;", errorMessage) &&
        sqliteExecWithError(database,
                            "CREATE TABLE IF NOT EXISTS backends ("
                            "service_id TEXT PRIMARY KEY,"
                            "display_name TEXT NOT NULL DEFAULT '',"
                            "icon TEXT,"
                            "icon_path TEXT,"
                            "service_unit TEXT,"
                            "unit_name TEXT NOT NULL DEFAULT '',"
                            "unit_path TEXT NOT NULL DEFAULT '',"
                            "owns_unit INTEGER NOT NULL DEFAULT 0"
                            ");",
                            errorMessage) &&
        registryTableHasColumn(database, "backends", "icon_path", hasBackendIconPathColumn, errorMessage) &&
        (hasBackendIconPathColumn ||
         sqliteExecWithError(database,
                             "ALTER TABLE backends ADD COLUMN icon_path TEXT;",
                             errorMessage)) &&
        sqliteExecWithError(database,
                            "UPDATE backends SET icon_path = icon "
                            "WHERE (icon_path IS NULL OR icon_path = '') "
                            "AND icon IS NOT NULL AND icon != '' AND substr(icon, 1, 5) != 'data:';",
                            errorMessage) &&
        registryTableHasColumn(database, "backends", "unit_name", hasBackendUnitNameColumn, errorMessage) &&
        (hasBackendUnitNameColumn ||
         sqliteExecWithError(database,
                             "ALTER TABLE backends ADD COLUMN unit_name TEXT NOT NULL DEFAULT '';",
                             errorMessage)) &&
        registryTableHasColumn(database, "backends", "unit_path", hasBackendUnitPathColumn, errorMessage) &&
        (hasBackendUnitPathColumn ||
         sqliteExecWithError(database,
                             "ALTER TABLE backends ADD COLUMN unit_path TEXT NOT NULL DEFAULT '';",
                             errorMessage)) &&
        registryTableHasColumn(database, "backends", "owns_unit", hasBackendOwnsUnitColumn, errorMessage) &&
        (hasBackendOwnsUnitColumn ||
         sqliteExecWithError(database,
                             "ALTER TABLE backends ADD COLUMN owns_unit INTEGER NOT NULL DEFAULT 0;",
                             errorMessage)) &&
        frontendsTableUsesLegacySchema(database, usesLegacyFrontendsTable, errorMessage) &&
        (!usesLegacyFrontendsTable ||
         sqliteExecWithError(database,
                             "DROP TABLE IF EXISTS frontends;",
                             errorMessage)) &&
        sqliteExecWithError(database,
                            "DROP INDEX IF EXISTS frontends_service_port_unique;",
                            errorMessage) &&
        sqliteExecWithError(database,
                            "CREATE TABLE IF NOT EXISTS frontends ("
                            "url TEXT PRIMARY KEY,"
                            "service_id TEXT,"
                            "display_name TEXT NOT NULL DEFAULT '',"
                            "port INTEGER NOT NULL DEFAULT 0,"
                            "socket_path TEXT NOT NULL DEFAULT '',"
                            "icon TEXT,"
                            "icon_path TEXT,"
                            "list TEXT"
                            ");",
                            errorMessage) &&
        sqliteExecWithError(database,
                            "CREATE INDEX IF NOT EXISTS frontends_service_id_idx "
                            "ON frontends(service_id);",
                            errorMessage) &&
        sqliteExecWithError(database,
                            "CREATE TABLE IF NOT EXISTS log_files ("
                            "path TEXT PRIMARY KEY,"
                            "service_id TEXT NOT NULL"
                            ");",
                            errorMessage) &&
        sqliteExecWithError(database,
                            "CREATE INDEX IF NOT EXISTS log_files_service_id_idx "
                            "ON log_files(service_id);",
                            errorMessage) &&
        ensurePlatformRegistrySchema(database, errorMessage) &&
        frontendsTableHasColumn(database, "display_name", hasFrontendDisplayNameColumn, errorMessage) &&
        (hasFrontendDisplayNameColumn ||
         sqliteExecWithError(database,
                             "ALTER TABLE frontends ADD COLUMN display_name TEXT NOT NULL DEFAULT '';",
                             errorMessage)) &&
        frontendsTableHasColumn(database, "name", hasFrontendNameColumn, errorMessage) &&
        (!hasFrontendNameColumn ||
         sqliteExecWithError(database,
                             "UPDATE frontends SET display_name = name WHERE display_name = '';",
                             errorMessage)) &&
        frontendsTableHasColumn(database, "socket_path", hasSocketPathColumn, errorMessage) &&
        (hasSocketPathColumn ||
         sqliteExecWithError(database,
                             "ALTER TABLE frontends ADD COLUMN socket_path TEXT NOT NULL DEFAULT '';",
                             errorMessage)) &&
        frontendsTableHasColumn(database, "icon_path", hasFrontendIconPathColumn, errorMessage) &&
        (hasFrontendIconPathColumn ||
         sqliteExecWithError(database,
                             "ALTER TABLE frontends ADD COLUMN icon_path TEXT;",
                             errorMessage)) &&
        sqliteExecWithError(database,
                            "UPDATE frontends SET icon_path = icon "
                            "WHERE (icon_path IS NULL OR icon_path = '') "
                            "AND icon IS NOT NULL AND icon != '' AND substr(icon, 1, 5) != 'data:';",
                            errorMessage) &&
        frontendsTableHasColumn(database, "list", hasListColumn, errorMessage) &&
        (hasListColumn ||
         sqliteExecWithError(database,
                             "ALTER TABLE frontends ADD COLUMN list TEXT;",
                             errorMessage)) &&
        sqliteExecWithError(database,
                            "CREATE TABLE IF NOT EXISTS frontend_layouts ("
                            "url TEXT PRIMARY KEY,"
                            "list TEXT NOT NULL DEFAULT ''"
                            ");",
                            errorMessage);
    if (!schemaReady) {
        return false;
    }
    if (hasFrontendNameColumn &&
        !sqliteExecWithError(database,
                             "DROP INDEX IF EXISTS frontends_service_id_idx;"
                             "CREATE TABLE frontends_new ("
                             "url TEXT PRIMARY KEY,"
                             "service_id TEXT,"
                             "display_name TEXT NOT NULL DEFAULT '',"
                             "port INTEGER NOT NULL DEFAULT 0,"
                             "socket_path TEXT NOT NULL DEFAULT '',"
                             "icon TEXT,"
                             "icon_path TEXT,"
                             "list TEXT"
                             ");"
                             "INSERT OR REPLACE INTO frontends_new(url, service_id, display_name, port, socket_path, icon, icon_path, list) "
                             "SELECT url, service_id, COALESCE(NULLIF(display_name, ''), name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), icon, icon_path, list FROM frontends;"
                             "DROP TABLE frontends;"
                             "ALTER TABLE frontends_new RENAME TO frontends;"
                             "CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);",
                             errorMessage)) {
        return false;
    }
    if (!hadFrontendLayoutsTable &&
        !sqliteExecWithError(database,
                             "INSERT OR IGNORE INTO frontend_layouts(url, list) "
                             "SELECT url, COALESCE(list, '') FROM frontends;",
                             errorMessage)) {
        return false;
    }
    return sqliteExecWithError(database,
                               "DELETE FROM frontend_layouts "
                               "WHERE url IN ("
                               "  SELECT raw.url FROM frontends raw "
                               "  JOIN frontends canonical "
                               "    ON canonical.service_id = raw.service_id "
                               "   AND canonical.socket_path = raw.socket_path "
                               "   AND canonical.url = raw.url || '/' "
                               "  WHERE raw.socket_path != '' AND raw.url = raw.socket_path"
                               ");"
                               "UPDATE frontend_layouts "
                               "SET url = url || '/' "
                               "WHERE EXISTS ("
                               "  SELECT 1 FROM frontends f "
                               "  WHERE f.url = frontend_layouts.url "
                               "    AND f.socket_path != '' "
                               "    AND f.url = f.socket_path"
                               ") "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM frontend_layouts existing "
                               "  WHERE existing.url = frontend_layouts.url || '/'"
                               ");"
                               "DELETE FROM frontends "
                               "WHERE socket_path != '' "
                               "AND url = socket_path "
                               "AND EXISTS ("
                               "  SELECT 1 FROM frontends canonical "
                               "  WHERE canonical.service_id = frontends.service_id "
                               "    AND canonical.socket_path = frontends.socket_path "
                               "    AND canonical.url = frontends.url || '/'"
                               ");"
                               "UPDATE frontends "
                               "SET url = url || '/' "
                               "WHERE socket_path != '' AND url = socket_path;",
                               errorMessage);
}

bool upsertBackendRegistryRecord(sqlite3* database,
                                 const char* identifier,
                                 const char* displayName,
                                 const char* iconPath,
                                 Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "INSERT INTO backends(service_id, display_name, icon_path, service_unit) "
                                "VALUES(?, ?, NULLIF(?, ''), NULL) "
                                "ON CONFLICT(service_id) DO UPDATE SET "
                                "display_name = excluded.display_name, "
                                "icon_path = COALESCE(excluded.icon_path, backends.icon_path);",
                                statement,
                                errorMessage)) {
        return false;
    }

    if (sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 2, displayName, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 3, iconPath ? iconPath : "", -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(statement);
        return setSqliteError(errorMessage, database, "Failed to bind backend registry values: ");
    }

    const bool ok = sqliteStepDone(statement, errorMessage);
    sqlite3_finalize(statement);
    return ok;
}

bool deleteBackendRegistryRecord(sqlite3* database,
                                 const char* identifier,
                                 Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "DELETE FROM backends WHERE service_id = ?;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to delete backend registry row: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool updateBackendUnitRegistryRecord(sqlite3* database,
                                     const char* identifier,
                                     const char* unitName,
                                     const char* unitPath,
                                     bool ownsUnit,
                                     Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "UPDATE backends SET service_unit = CASE WHEN ? = '' THEN '' ELSE service_unit END, "
                                "unit_name = ?, unit_path = ?, owns_unit = ? WHERE service_id = ?;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, unitName ? unitName : "", -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, unitName ? unitName : "", -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 3, unitPath ? unitPath : "", -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int(statement, 4, ownsUnit ? 1 : 0) == SQLITE_OK &&
        sqlite3_bind_text(statement, 5, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to update backend unit registry row: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool backendRegistryRecordExists(sqlite3* database,
                                 const char* identifier,
                                 bool& exists,
                                 Buffer& errorMessage) {
    exists = false;
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "SELECT 1 FROM backends WHERE service_id = ? LIMIT 1;",
                                statement,
                                errorMessage)) {
        return false;
    }

    if (sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(statement);
        return setSqliteError(errorMessage, database, "Failed to bind backend lookup identifier: ");
    }

    const int stepResult = sqlite3_step(statement);
    if (stepResult == SQLITE_ROW) {
        exists = true;
        sqlite3_finalize(statement);
        return true;
    }
    if (stepResult != SQLITE_DONE) {
        sqlite3_finalize(statement);
        return setSqliteError(errorMessage, database, "Failed to query backend registry rows: ");
    }

    sqlite3_finalize(statement);
    return true;
}

bool countRegistryRows(sqlite3* database,
                       const char* sql,
                       const char* identifier,
                       int& countOut,
                       Buffer& errorMessage) {
    countOut = 0;
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database, sql, statement, errorMessage)) {
        return false;
    }

    if (sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(statement);
        return setSqliteError(errorMessage, database, "Failed to bind registry count identifier: ");
    }

    const int stepResult = sqlite3_step(statement);
    if (stepResult != SQLITE_ROW) {
        sqlite3_finalize(statement);
        if (stepResult == SQLITE_DONE) {
            countOut = 0;
            return true;
        }
        return setSqliteError(errorMessage, database, "Failed to query registry row count: ");
    }

    countOut = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return true;
}

bool upsertLaunchdBackendRegistryRecord(sqlite3* database,
                                        const char* identifier,
                                        const char* plistPath,
                                        bool ownsPlist,
                                        Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "INSERT INTO launchd_backends(service_id, plist_path, owns_plist) "
                                "VALUES(?, ?, ?) "
                                "ON CONFLICT(service_id) DO UPDATE SET "
                                "plist_path = excluded.plist_path, "
                                "owns_plist = excluded.owns_plist;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, plistPath, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int(statement, 3, ownsPlist ? 1 : 0) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to upsert launchd registry row: ");
    }
    sqlite3_finalize(statement);
    return ok && updateBackendUnitRegistryRecord(database, identifier, identifier, plistPath, ownsPlist, errorMessage);
}

bool deleteLaunchdBackendRegistryRecord(sqlite3* database,
                                        const char* identifier,
                                        Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "DELETE FROM launchd_backends WHERE service_id = ?;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to delete launchd registry row: ");
    }
    sqlite3_finalize(statement);
    return ok && updateBackendUnitRegistryRecord(database, identifier, "", "", false, errorMessage);
}

bool upsertSystemdBackendRegistryRecord(sqlite3* database,
                                        const char* identifier,
                                        const char* unitName,
                                        Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "INSERT INTO systemd_backends(service_id, unit_name) "
                                "VALUES(?, ?) "
                                "ON CONFLICT(service_id) DO UPDATE SET "
                                "unit_name = excluded.unit_name;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, unitName, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to upsert systemd registry row: ");
    }
    sqlite3_finalize(statement);
    return ok && updateBackendUnitRegistryRecord(database, identifier, unitName, "", true, errorMessage);
}

bool deleteSystemdBackendRegistryRecord(sqlite3* database,
                                        const char* identifier,
                                        Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "DELETE FROM systemd_backends WHERE service_id = ?;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to delete systemd registry row: ");
    }
    sqlite3_finalize(statement);
    return ok && updateBackendUnitRegistryRecord(database, identifier, "", "", false, errorMessage);
}

bool buildLogPathForIdentifier(const char* identifier, Buffer& path) {
    clearBuffer(path);
    if (!resolveOuterwebappsHome(path)) {
        return false;
    }
    if (strcmp(identifier, "outerwebapps") == 0) {
        return appendPathComponent(path, "backend.log");
    }
    return appendPathComponent(path, identifier) &&
        appendPathComponent(path, "backend.log");
}

bool upsertLogFileRecord(sqlite3* database,
                         const char* identifier,
                         Buffer& errorMessage) {
    Buffer path;
    initBuffer(path);
    const bool built = buildLogPathForIdentifier(identifier, path);
    if (!built) {
        freeBuffer(path);
        return setError(errorMessage, "Out of memory.");
    }

    sqlite3_stmt* statement = nullptr;
    const bool prepared = sqlitePrepareWithError(database,
                                                 "INSERT INTO log_files(path, service_id) VALUES(?, ?) "
                                                 "ON CONFLICT(path) DO UPDATE SET service_id = excluded.service_id;",
                                                 statement,
                                                 errorMessage);
    if (!prepared) {
        freeBuffer(path);
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, path.data, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to upsert log file record: ");
    }
    sqlite3_finalize(statement);
    freeBuffer(path);
    return ok;
}

bool insertLogFileRegistryRecord(sqlite3* database,
                                 const char* identifier,
                                 const char* path,
                                 Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "INSERT INTO log_files(path, service_id) VALUES(?, ?) "
                                "ON CONFLICT(path) DO UPDATE SET service_id = excluded.service_id;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, path, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to upsert log file record: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool deleteLogFileRegistryRecord(sqlite3* database,
                                 const char* identifier,
                                 const char* path,
                                 Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "DELETE FROM log_files WHERE service_id = ? AND path = ?;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to delete log file record: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool clearLogFileRegistryRecords(sqlite3* database,
                                 const char* identifier,
                                 Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "DELETE FROM log_files WHERE service_id = ?;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to clear log file records: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool clearFrontendRegistryRecords(sqlite3* database,
                                  const char* identifier,
                                  Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "DELETE FROM frontends WHERE service_id = ?;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to delete frontend registry rows: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool insertFrontendRegistryRecord(sqlite3* database,
                                  const char* identifier,
                                  const HostedAppEntry& entry,
                                  Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "INSERT INTO frontends(service_id, display_name, port, url, socket_path, icon_path, list) "
                                "VALUES(?, ?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, '')) "
                                "ON CONFLICT(url) DO UPDATE SET "
                                "service_id = excluded.service_id, "
                                "display_name = excluded.display_name, "
                                "port = excluded.port, "
                                "socket_path = excluded.socket_path, "
                                "icon_path = excluded.icon_path, "
                                "list = excluded.list;",
                                statement,
                                errorMessage)) {
        return false;
    }

    const char* socketPath = (!entry.socketPath.data || entry.socketPath.size == 0) ? "" : entry.socketPath.data;
    const char* iconPath = (!entry.iconPath.data || entry.iconPath.size == 0 || strncmp(entry.iconPath.data, "data:", 5) == 0)
        ? ""
        : entry.iconPath.data;
    const int bindSocketPathResult =
        sqlite3_bind_text(statement, 5, socketPath, -1, SQLITE_TRANSIENT);
    const int bindIconResult = iconPath[0] == '\0'
        ? sqlite3_bind_null(statement, 6)
        : sqlite3_bind_text(statement, 6, iconPath, -1, SQLITE_TRANSIENT);
    const int bindListResult = (!entry.list.data || entry.list.size == 0)
        ? sqlite3_bind_null(statement, 7)
        : sqlite3_bind_text(statement, 7, entry.list.data, static_cast<int>(entry.list.size), SQLITE_TRANSIENT);
    const bool ok =
        sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, entry.name.data ? entry.name.data : "", static_cast<int>(entry.name.size), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int(statement, 3, entry.port) == SQLITE_OK &&
        sqlite3_bind_text(statement, 4, entry.url.data ? entry.url.data : "/", static_cast<int>(entry.url.size), SQLITE_TRANSIENT) == SQLITE_OK &&
        bindSocketPathResult == SQLITE_OK &&
        bindIconResult == SQLITE_OK &&
        bindListResult == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to insert frontend registry row: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool insertFrontendLayoutRegistryRecord(sqlite3* database,
                                        const char* url,
                                        const char* list,
                                        Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "INSERT OR REPLACE INTO frontend_layouts(url, list) VALUES(?, ?);",
                                statement,
                                errorMessage)) {
        return false;
    }
    const bool ok =
        sqlite3_bind_text(statement, 1, url ? url : "", -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, list ? list : "", -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to insert frontend layout registry row: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool loadHostedAppsFromDatabase(sqlite3* database,
                                const char* identifier,
                                HostedAppArray& apps,
                                Buffer& errorMessage) {
    freeHostedAppArray(apps);
    initHostedAppArray(apps);
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "SELECT display_name, port, COALESCE(socket_path, ''), url, COALESCE(icon_path, ''), COALESCE(list, '') "
                                "FROM frontends WHERE service_id = ? "
                                "ORDER BY COALESCE(socket_path, ''), port, url;",
                                statement,
                                errorMessage)) {
        return false;
    }

    if (sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(statement);
        return setSqliteError(errorMessage, database, "Failed to bind frontend lookup identifier: ");
    }

    while (true) {
        const int stepResult = sqlite3_step(statement);
        if (stepResult == SQLITE_DONE) {
            break;
        }
        if (stepResult != SQLITE_ROW) {
            sqlite3_finalize(statement);
            return setSqliteError(errorMessage, database, "Failed to load frontend registry rows: ");
        }

        HostedAppEntry entry;
        initHostedAppEntry(entry);
        entry.port = sqlite3_column_int(statement, 1);
        const char* socketPath = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        const char* url = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
        const char* iconPath = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
        const char* list = reinterpret_cast<const char*>(sqlite3_column_text(statement, 5));
        if (!assignCString(entry.name, name ? name : "") ||
            !assignCString(entry.socketPath, socketPath ? socketPath : "") ||
            !assignHostedAppURL(entry.url, url, entry.port, socketPath) ||
            !assignCString(entry.iconPath, iconPath ? iconPath : "") ||
            !assignCString(entry.list, list ? list : "") ||
            !appendHostedAppMove(apps, entry)) {
            freeHostedAppEntry(entry);
            sqlite3_finalize(statement);
            return setError(errorMessage, "Out of memory.");
        }
    }

    sqlite3_finalize(statement);
    return true;
}

bool replaceHostedAppsInDatabase(sqlite3* database,
                                 const char* identifier,
                                 const HostedAppArray& apps,
                                 Buffer& errorMessage) {
    if (!sqliteExecWithError(database, "BEGIN IMMEDIATE TRANSACTION;", errorMessage)) {
        return false;
    }

    bool ok = true;
    if (ok) {
        ok = clearFrontendRegistryRecords(database, identifier, errorMessage);
    }
    for (size_t i = 0; ok && i < apps.count; i += 1) {
        ok = insertFrontendRegistryRecord(database, identifier, apps.items[i], errorMessage);
    }

    if (ok) {
        ok = sqliteExecWithError(database, "COMMIT;", errorMessage);
    } else {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (!ok) {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

bool verifyHostedAppsInDatabase(sqlite3* database,
                                const char* identifier,
                                size_t expectedCount,
                                Buffer& errorMessage) {
    int frontendCount = 0;
    if (!countRegistryRows(database,
                           "SELECT COUNT(*) FROM frontends WHERE service_id = ?;",
                           identifier,
                           frontendCount,
                           errorMessage)) {
        return false;
    }

    if (frontendCount == static_cast<int>(expectedCount)) {
        return true;
    }

    char message[256];
    snprintf(message,
             sizeof(message),
             "Hosted app verification failed for backend '%s': expected %zu frontend rows, found %d.",
             identifier ? identifier : "",
             expectedCount,
             frontendCount);
    return setError(errorMessage, message);
}

const char* sqliteColumnTextOrEmpty(sqlite3_stmt* statement, int column) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return text ? text : "";
}

bool sqliteTableExists(sqlite3* database, const char* tableName, bool& exists, Buffer& errorMessage) {
    exists = false;
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1;",
                                statement,
                                errorMessage)) {
        return false;
    }
    if (sqlite3_bind_text(statement, 1, tableName, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(statement);
        return setSqliteError(errorMessage, database, "Failed to bind table lookup name: ");
    }
    const int stepResult = sqlite3_step(statement);
    if (stepResult == SQLITE_ROW) {
        exists = true;
        sqlite3_finalize(statement);
        return true;
    }
    if (stepResult != SQLITE_DONE) {
        sqlite3_finalize(statement);
        return setSqliteError(errorMessage, database, "Failed to inspect registry table: ");
    }
    sqlite3_finalize(statement);
    return true;
}

bool registryBinaryCountRows(sqlite3* database, const char* tableName, uint64_t& count, Buffer& errorMessage) {
    count = 0;
    bool exists = false;
    if (!sqliteTableExists(database, tableName, exists, errorMessage)) {
        return false;
    }
    if (!exists) {
        return true;
    }

    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", tableName);
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database, sql, statement, errorMessage)) {
        return false;
    }
    const int stepResult = sqlite3_step(statement);
    if (stepResult == SQLITE_ROW) {
        const sqlite3_int64 value = sqlite3_column_int64(statement, 0);
        count = value > 0 ? static_cast<uint64_t>(value) : 0;
        sqlite3_finalize(statement);
        return true;
    }
    sqlite3_finalize(statement);
    if (stepResult == SQLITE_DONE) {
        return true;
    }
    return setSqliteError(errorMessage, database, "Failed to count registry rows: ");
}

bool appendRegistryBinaryStringRef(OrwaStringPool& pool,
                                   Buffer& variableRegion,
                                   Buffer& rows,
                                   const char* rawText) {
    const char* text = rawText ? rawText : "";
    const size_t length = strlen(text);
    if (length == 0) {
        return appendLittleEndianUInt64(rows, 0) &&
            appendLittleEndianUInt64(rows, 0);
    }

    for (const OrwaStringEntry& entry : pool.entries) {
        if (entry.value.size() == length && memcmp(entry.value.data(), text, length) == 0) {
            return appendLittleEndianUInt64(rows, entry.offset) &&
                appendLittleEndianUInt64(rows, static_cast<uint64_t>(length));
        }
    }

    const uint64_t offset = pool.variableBaseOffset + static_cast<uint64_t>(variableRegion.size);
    if (!appendBuffer(variableRegion, text, length)) {
        return false;
    }
    OrwaStringEntry entry;
    entry.value.assign(text, length);
    entry.offset = offset;
    pool.entries.push_back(entry);
    return appendLittleEndianUInt64(rows, offset) &&
        appendLittleEndianUInt64(rows, static_cast<uint64_t>(length));
}

bool registryBinaryAppendQuery(sqlite3* database,
                               const char* sql,
                               int expectedColumns,
                               bool (*appendRow)(sqlite3_stmt*, OrwaStringPool&, Buffer&, Buffer&),
                               OrwaStringPool& pool,
                               Buffer& variableRegion,
                               Buffer& rows,
                               Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database, sql, statement, errorMessage)) {
        return false;
    }
    if (sqlite3_column_count(statement) != expectedColumns) {
        sqlite3_finalize(statement);
        return setError(errorMessage, "Unexpected column count while exporting registry.");
    }

    while (true) {
        const int stepResult = sqlite3_step(statement);
        if (stepResult == SQLITE_DONE) {
            sqlite3_finalize(statement);
            return true;
        }
        if (stepResult != SQLITE_ROW) {
            sqlite3_finalize(statement);
            return setSqliteError(errorMessage, database, "Failed to export registry rows: ");
        }
        if (!appendRow(statement, pool, variableRegion, rows)) {
            sqlite3_finalize(statement);
            return setError(errorMessage, "Out of memory while exporting registry.");
        }
    }
}

bool registryBinaryAppendBackendRow(sqlite3_stmt* statement,
                                    OrwaStringPool& pool,
                                    Buffer& variableRegion,
                                    Buffer& rows) {
    const uint32_t flags = sqlite3_column_int(statement, 5) != 0 ? 1u : 0u;
    return appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 0)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 1)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 2)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 3)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 4)) &&
        appendLittleEndianUInt32(rows, flags);
}

bool registryBinaryAppendFrontendRow(sqlite3_stmt* statement,
                                     OrwaStringPool& pool,
                                     Buffer& variableRegion,
                                     Buffer& rows) {
    const char* socketPath = sqliteColumnTextOrEmpty(statement, 4);
    const uint32_t port = static_cast<uint32_t>(sqlite3_column_int(statement, 3));
    const uint8_t endpointKind = socketPath[0] ? 2u : (port ? 1u : 0u);
    const uint8_t zeroPadding[12] = {};
    const uint8_t emptyPayload[16] = {};
    const bool ok = appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 0)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 1)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 2)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 5)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 6)) &&
        appendBuffer(rows, &endpointKind, sizeof(endpointKind));
    if (!ok) {
        return false;
    }
    if (endpointKind == 2u) {
        return appendRegistryBinaryStringRef(pool, variableRegion, rows, socketPath);
    }
    if (endpointKind == 1u) {
        return appendLittleEndianUInt32(rows, port) &&
            appendBuffer(rows, zeroPadding, sizeof(zeroPadding));
    }
    return appendBuffer(rows, emptyPayload, sizeof(emptyPayload));
}

bool registryBinaryAppendFrontendLayoutRow(sqlite3_stmt* statement,
                                           OrwaStringPool& pool,
                                           Buffer& variableRegion,
                                           Buffer& rows) {
    return appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 0)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 1));
}

bool registryBinaryAppendLogFileRow(sqlite3_stmt* statement,
                                    OrwaStringPool& pool,
                                    Buffer& variableRegion,
                                    Buffer& rows) {
    return appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 0)) &&
        appendRegistryBinaryStringRef(pool, variableRegion, rows, sqliteColumnTextOrEmpty(statement, 1));
}

bool writeRegistryBinaryFile(const char* path, const void* data, size_t length, Buffer& errorMessage) {
    Buffer tempPath;
    initBuffer(tempPath);
    const bool haveTempPath = appendCString(tempPath, path) && appendCString(tempPath, ".tmp.XXXXXX");
    if (!haveTempPath) {
        freeBuffer(tempPath);
        return setError(errorMessage, "Out of memory.");
    }

    int fd = mkstemp(tempPath.data);
    if (fd < 0) {
        freeBuffer(tempPath);
        return setErrnoError(errorMessage, "Failed to open registry binary temp file: ");
    }
    if (fchmod(fd, 0644) != 0) {
        setErrnoError(errorMessage, "Failed to chmod registry binary temp file: ");
        close(fd);
        unlink(tempPath.data);
        freeBuffer(tempPath);
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    bool ok = true;
    while (written < length) {
        const ssize_t result = write(fd, bytes + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (result == 0) {
            ok = false;
            break;
        }
        written += static_cast<size_t>(result);
    }
    if (ok && fsync(fd) != 0) {
        ok = false;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        setErrnoError(errorMessage, "Failed to write registry binary: ");
        unlink(tempPath.data);
        freeBuffer(tempPath);
        return false;
    }
    if (rename(tempPath.data, path) != 0) {
        setErrnoError(errorMessage, "Failed to publish registry binary: ");
        unlink(tempPath.data);
        freeBuffer(tempPath);
        return false;
    }
    Buffer directoryPath;
    initBuffer(directoryPath);
    if (appendCString(directoryPath, path)) {
        char* slash = strrchr(directoryPath.data, '/');
        if (slash) {
            if (slash == directoryPath.data) {
                slash[1] = '\0';
            } else {
                *slash = '\0';
            }
            int dirfd = open(directoryPath.data, O_RDONLY);
            if (dirfd >= 0) {
                (void)fsync(dirfd);
                close(dirfd);
            }
        }
    }
    freeBuffer(directoryPath);
    freeBuffer(tempPath);
    return true;
}

bool exportRegistryBinaryFromDatabase(sqlite3* database, Buffer& errorMessage) {
    uint64_t counts[ORWA_TABLE_COUNT] = {};
    OrwaTableDescriptor descriptors[ORWA_TABLE_COUNT] = {};
    if (!registryBinaryCountRows(database, "backends", counts[ORWA_TABLE_BACKENDS], errorMessage) ||
        !registryBinaryCountRows(database, "frontends", counts[ORWA_TABLE_FRONTENDS], errorMessage) ||
        !registryBinaryCountRows(database, "frontend_layouts", counts[ORWA_TABLE_FRONTEND_LAYOUTS], errorMessage) ||
        !registryBinaryCountRows(database, "log_files", counts[ORWA_TABLE_LOG_FILES], errorMessage)) {
        return false;
    }

    const uint32_t rowSizes[ORWA_TABLE_COUNT] = {
        ORWA_BACKENDS_ROW_SIZE,
        ORWA_FRONTENDS_ROW_SIZE,
        ORWA_FRONTEND_LAYOUTS_ROW_SIZE,
        ORWA_LOG_FILES_ROW_SIZE
    };
    uint64_t offset = ORWA_HEADER_SIZE;
    for (size_t i = 0; i < ORWA_TABLE_COUNT; i += 1) {
        descriptors[i].offset = offset;
        descriptors[i].rowCount = counts[i];
        descriptors[i].rowSize = rowSizes[i];
        offset += counts[i] * rowSizes[i];
    }

    Buffer rows;
    Buffer variableRegion;
    Buffer file;
    Buffer outputPath;
    initBuffer(rows);
    initBuffer(variableRegion);
    initBuffer(file);
    initBuffer(outputPath);
    OrwaStringPool pool;
    pool.variableBaseOffset = offset;

    bool ok = true;
    if (ok && counts[ORWA_TABLE_BACKENDS] > 0) {
        bool hasSystemdTable = false;
        bool hasLaunchdTable = false;
        ok = sqliteTableExists(database, "systemd_backends", hasSystemdTable, errorMessage) &&
            sqliteTableExists(database, "launchd_backends", hasLaunchdTable, errorMessage);
        const char* systemdJoin = hasSystemdTable ? "LEFT JOIN systemd_backends s ON s.service_id = b.service_id" : "";
        const char* launchdJoin = hasLaunchdTable ? "LEFT JOIN launchd_backends l ON l.service_id = b.service_id" : "";
        const char* unitNameExpression = "COALESCE(NULLIF(b.unit_name, ''), NULLIF(b.service_unit, ''), '')";
        const char* unitPathExpression = "COALESCE(b.unit_path, '')";
        const char* ownsUnitExpression = "CASE WHEN COALESCE(b.owns_unit, 0) != 0 THEN 1 WHEN COALESCE(b.service_unit, '') != '' THEN 1 ELSE 0 END";
        if (hasSystemdTable && hasLaunchdTable) {
            unitNameExpression = "COALESCE(NULLIF(b.unit_name, ''), CASE WHEN COALESCE(l.plist_path, '') != '' THEN b.service_id ELSE NULL END, NULLIF(s.unit_name, ''), NULLIF(b.service_unit, ''), '')";
            unitPathExpression = "COALESCE(NULLIF(b.unit_path, ''), l.plist_path, '')";
            ownsUnitExpression = "CASE WHEN COALESCE(b.owns_unit, 0) != 0 THEN 1 WHEN COALESCE(l.plist_path, '') != '' THEN COALESCE(l.owns_plist, 0) WHEN COALESCE(s.unit_name, '') != '' OR COALESCE(b.service_unit, '') != '' THEN 1 ELSE 0 END";
        } else if (hasSystemdTable) {
            unitNameExpression = "COALESCE(NULLIF(b.unit_name, ''), NULLIF(s.unit_name, ''), NULLIF(b.service_unit, ''), '')";
            ownsUnitExpression = "CASE WHEN COALESCE(b.owns_unit, 0) != 0 THEN 1 WHEN COALESCE(s.unit_name, '') != '' OR COALESCE(b.service_unit, '') != '' THEN 1 ELSE 0 END";
        } else if (hasLaunchdTable) {
            unitNameExpression = "COALESCE(NULLIF(b.unit_name, ''), CASE WHEN COALESCE(l.plist_path, '') != '' THEN b.service_id ELSE NULL END, NULLIF(b.service_unit, ''), '')";
            unitPathExpression = "COALESCE(NULLIF(b.unit_path, ''), l.plist_path, '')";
            ownsUnitExpression = "CASE WHEN COALESCE(b.owns_unit, 0) != 0 THEN 1 WHEN COALESCE(l.plist_path, '') != '' THEN COALESCE(l.owns_plist, 0) WHEN COALESCE(b.service_unit, '') != '' THEN 1 ELSE 0 END";
        }
        char* sql = ok ? sqlite3_mprintf("SELECT b.service_id, COALESCE(b.display_name, ''), COALESCE(b.icon_path, ''), %s, %s, %s FROM backends b %s %s ORDER BY b.service_id;",
                                         unitNameExpression,
                                         unitPathExpression,
                                         ownsUnitExpression,
                                         systemdJoin,
                                         launchdJoin) : nullptr;
        if (ok && !sql) {
            ok = setError(errorMessage, "Out of memory while exporting registry.");
        }
        if (ok) {
            ok = registryBinaryAppendQuery(database,
                                           sql,
                                           6,
                                           registryBinaryAppendBackendRow,
                                           pool,
                                           variableRegion,
                                           rows,
                                           errorMessage);
        }
        sqlite3_free(sql);
    }
    if (ok && counts[ORWA_TABLE_FRONTENDS] > 0) {
        ok = registryBinaryAppendQuery(database,
                                       "SELECT url, COALESCE(service_id, ''), COALESCE(display_name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), COALESCE(icon_path, ''), COALESCE(list, '') FROM frontends ORDER BY service_id, COALESCE(list, ''), display_name, url;",
                                       7,
                                       registryBinaryAppendFrontendRow,
                                       pool,
                                       variableRegion,
                                       rows,
                                       errorMessage);
    }
    if (ok && counts[ORWA_TABLE_FRONTEND_LAYOUTS] > 0) {
        ok = registryBinaryAppendQuery(database,
                                       "SELECT url, COALESCE(list, '') FROM frontend_layouts ORDER BY url;",
                                       2,
                                       registryBinaryAppendFrontendLayoutRow,
                                       pool,
                                       variableRegion,
                                       rows,
                                       errorMessage);
    }
    if (ok && counts[ORWA_TABLE_LOG_FILES] > 0) {
        ok = registryBinaryAppendQuery(database,
                                       "SELECT path, service_id FROM log_files ORDER BY service_id, path;",
                                       2,
                                       registryBinaryAppendLogFileRow,
                                       pool,
                                       variableRegion,
                                       rows,
                                       errorMessage);
    }
    if (ok && rows.size != offset - ORWA_HEADER_SIZE) {
        ok = setError(errorMessage, "Registry binary row length mismatch.");
    }

    if (ok) {
        ok = appendBuffer(file, "ORWA", 4) &&
            appendLittleEndianUInt32(file, 1);
    }
    for (size_t i = 0; ok && i < ORWA_TABLE_COUNT; i += 1) {
        ok = appendLittleEndianUInt64(file, descriptors[i].offset) &&
            appendLittleEndianUInt64(file, descriptors[i].rowCount) &&
            appendLittleEndianUInt32(file, descriptors[i].rowSize);
    }
    if (ok && file.size != ORWA_HEADER_SIZE) {
        ok = setError(errorMessage, "Registry binary header length mismatch.");
    }
    if (ok) {
        ok = appendBuffer(file, rows.data ? rows.data : "", rows.size) &&
            appendBuffer(file, variableRegion.data ? variableRegion.data : "", variableRegion.size);
    }
    if (ok && !buildOuterwebappsPath(kRegistryBinaryFileName, outputPath)) {
        ok = setError(errorMessage, "Out of memory.");
    }
    if (ok) {
        ok = writeRegistryBinaryFile(outputPath.data, file.data, file.size, errorMessage);
    }

    freeBuffer(rows);
    freeBuffer(variableRegion);
    freeBuffer(file);
    freeBuffer(outputPath);
    return ok;
}

bool readRegistryBinaryString(const Buffer& file,
                              uint64_t variableOffset,
                              uint64_t offset,
                              uint64_t length,
                              std::string& value,
                              Buffer& errorMessage) {
    value.clear();
    if (offset == 0 && length == 0) {
        return true;
    }
    if (offset < variableOffset ||
        offset > static_cast<uint64_t>(file.size) ||
        length > static_cast<uint64_t>(file.size) - offset) {
        return setError(errorMessage, "Registry binary string reference is out of bounds.");
    }
    value.assign(file.data + offset, file.data + offset + length);
    return true;
}

bool insertRegistryBinaryBackend(sqlite3* database,
                                 const std::string& serviceID,
                                 const std::string& displayName,
                                 const std::string& iconPath,
                                 const std::string& unitName,
                                 const std::string& unitPath,
                                 bool ownsUnit,
                                 Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database,
                                "INSERT INTO backends(service_id, display_name, icon_path, service_unit, unit_name, unit_path, owns_unit) "
                                "VALUES(?, ?, NULLIF(?, ''), NULLIF(?, ''), ?, ?, ?);",
                                statement,
                                errorMessage)) {
        return false;
    }
    const bool ok =
        sqlite3_bind_text(statement, 1, serviceID.data(), static_cast<int>(serviceID.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, displayName.data(), static_cast<int>(displayName.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 3, iconPath.data(), static_cast<int>(iconPath.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 4, unitName.data(), static_cast<int>(unitName.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 5, unitName.data(), static_cast<int>(unitName.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 6, unitPath.data(), static_cast<int>(unitPath.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int(statement, 7, ownsUnit ? 1 : 0) == SQLITE_OK &&
        sqliteStepDone(statement, errorMessage);
    if (!ok && errorMessage.size == 0) {
        setSqliteError(errorMessage, database, "Failed to import backend registry row: ");
    }
    sqlite3_finalize(statement);
    return ok;
}

bool importRegistryBinaryIntoDatabaseIfPresent(sqlite3* database, Buffer& errorMessage) {
    Buffer path;
    Buffer file;
    initBuffer(path);
    initBuffer(file);
    if (!buildOuterwebappsPath(kRegistryBinaryFileName, path)) {
        freeBuffer(path);
        freeBuffer(file);
        return setError(errorMessage, "Out of memory.");
    }
    if (!loadFile(path, file, errorMessage)) {
        freeBuffer(path);
        freeBuffer(file);
        return false;
    }
    freeBuffer(path);
    if (file.size == 0) {
        freeBuffer(file);
        return true;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(file.data);
    if (file.size < ORWA_LEGACY_THREE_TABLE_HEADER_SIZE ||
        memcmp(file.data, "ORWA", 4) != 0 ||
        readLittleEndianUInt32(bytes + 4) != 1) {
        freeBuffer(file);
        return setError(errorMessage, "Registry binary has an unsupported header.");
    }

    OrwaTableDescriptor descriptors[ORWA_LEGACY_TABLE_COUNT] = {};
    const bool usesLegacyHeader = file.size >= ORWA_LEGACY_HEADER_SIZE &&
        readLittleEndianUInt32(bytes + 8) == ORWA_LEGACY_HEADER_SIZE &&
        readLittleEndianUInt32(bytes + 40) == ORWA_LEGACY_TABLE_COUNT;
    const uint64_t firstTableOffset = usesLegacyHeader ? ORWA_LEGACY_HEADER_SIZE : readLittleEndianUInt64(bytes + 8);
    const size_t tableCount = usesLegacyHeader || firstTableOffset == ORWA_LEGACY_FIVE_TABLE_HEADER_SIZE ? ORWA_LEGACY_TABLE_COUNT :
        firstTableOffset == ORWA_HEADER_SIZE ? ORWA_TABLE_COUNT :
        firstTableOffset == ORWA_LEGACY_THREE_TABLE_HEADER_SIZE ? ORWA_LEGACY_THREE_TABLE_COUNT :
        0;
    if (tableCount == 0) {
        freeBuffer(file);
        return setError(errorMessage, "Registry binary has an unsupported table layout.");
    }
    const size_t descriptorBaseOffset = usesLegacyHeader ? 48 : 8;
    const size_t descriptorSize = usesLegacyHeader ? ORWA_LEGACY_TABLE_DESCRIPTOR_SIZE : ORWA_TABLE_DESCRIPTOR_SIZE;
    uint64_t variableOffset = 0;
    for (size_t i = 0; i < tableCount; i += 1) {
        const size_t descriptorOffset = descriptorBaseOffset + i * descriptorSize;
        if (descriptorOffset + descriptorSize > file.size) {
            freeBuffer(file);
            return setError(errorMessage, "Registry binary table descriptor is truncated.");
        }
        descriptors[i].offset = readLittleEndianUInt64(bytes + descriptorOffset);
        descriptors[i].rowCount = readLittleEndianUInt64(bytes + descriptorOffset + 8);
        descriptors[i].rowSize = readLittleEndianUInt32(bytes + descriptorOffset + 16);
        uint32_t expectedRowSize = 0;
        if (i == ORWA_TABLE_BACKENDS) {
            expectedRowSize = tableCount == ORWA_TABLE_COUNT ? ORWA_BACKENDS_ROW_SIZE : ORWA_LEGACY_BACKENDS_ROW_SIZE;
        } else if (i == ORWA_TABLE_FRONTENDS) {
            expectedRowSize = ORWA_FRONTENDS_ROW_SIZE;
        } else if (i == ORWA_TABLE_FRONTEND_LAYOUTS && tableCount == ORWA_TABLE_COUNT) {
            expectedRowSize = ORWA_FRONTEND_LAYOUTS_ROW_SIZE;
        } else if ((tableCount == ORWA_TABLE_COUNT && i == ORWA_TABLE_LOG_FILES) ||
                   (tableCount != ORWA_TABLE_COUNT && i == ORWA_TABLE_FRONTEND_LAYOUTS)) {
            expectedRowSize = ORWA_LOG_FILES_ROW_SIZE;
        } else if (i == ORWA_LEGACY_TABLE_SYSTEMD_BACKENDS) {
            expectedRowSize = ORWA_LEGACY_SYSTEMD_BACKENDS_ROW_SIZE_CURRENT;
        } else if (i == ORWA_LEGACY_TABLE_LAUNCHD_BACKENDS) {
            expectedRowSize = ORWA_LAUNCHD_BACKENDS_ROW_SIZE;
        }
        const bool rowSizeMatches = descriptors[i].rowSize == expectedRowSize ||
            (i == ORWA_TABLE_FRONTENDS && descriptors[i].rowSize == ORWA_LEGACY_FRONTENDS_ROW_SIZE) ||
            (i == ORWA_LEGACY_TABLE_SYSTEMD_BACKENDS && descriptors[i].rowSize == ORWA_LEGACY_SYSTEMD_BACKENDS_ROW_SIZE);
        if (!rowSizeMatches ||
            descriptors[i].offset > static_cast<uint64_t>(file.size) ||
            descriptors[i].rowCount > (UINT64_MAX / descriptors[i].rowSize) ||
            descriptors[i].rowCount * descriptors[i].rowSize > static_cast<uint64_t>(file.size) - descriptors[i].offset) {
            freeBuffer(file);
            return setError(errorMessage, "Registry binary table descriptor is invalid.");
        }
        const uint64_t tableEnd = descriptors[i].offset + descriptors[i].rowCount * descriptors[i].rowSize;
        if (tableEnd > variableOffset) {
            variableOffset = tableEnd;
        }
    }

    if (!sqliteExecWithError(database, "BEGIN IMMEDIATE TRANSACTION;", errorMessage)) {
        freeBuffer(file);
        return false;
    }

    bool ok = sqliteExecWithError(database, "DELETE FROM frontend_layouts;", errorMessage) &&
        sqliteExecWithError(database, "DELETE FROM frontends;", errorMessage) &&
        sqliteExecWithError(database, "DELETE FROM log_files;", errorMessage) &&
        sqliteExecWithError(database, "DELETE FROM backends;", errorMessage);
    if (ok && registrySupportsSystemdBackends()) {
        ok = sqliteExecWithError(database, "DELETE FROM systemd_backends;", errorMessage);
    }
    if (ok && registrySupportsLaunchdBackends()) {
        ok = sqliteExecWithError(database, "DELETE FROM launchd_backends;", errorMessage);
    }

    std::string a;
    std::string b;
    std::string c;
    std::string d;
    std::string e;
    std::string f;

    OrwaTableDescriptor& backendTable = descriptors[ORWA_TABLE_BACKENDS];
    for (uint64_t row = 0; ok && row < backendTable.rowCount; row += 1) {
        const uint64_t rowOffset = backendTable.offset + row * backendTable.rowSize;
        const uint8_t* rowBytes = bytes + rowOffset;
        ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes), readLittleEndianUInt64(rowBytes + 8), a, errorMessage) &&
            readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 16), readLittleEndianUInt64(rowBytes + 24), b, errorMessage) &&
            readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 32), readLittleEndianUInt64(rowBytes + 40), c, errorMessage) &&
            readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 48), readLittleEndianUInt64(rowBytes + 56), d, errorMessage);
        bool ownsUnit = !d.empty();
        e.clear();
        if (ok && backendTable.rowSize == ORWA_BACKENDS_ROW_SIZE) {
            ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 64), readLittleEndianUInt64(rowBytes + 72), e, errorMessage);
            ownsUnit = (readLittleEndianUInt32(rowBytes + 80) & 1u) != 0;
        }
        if (c.rfind("data:", 0) == 0) {
            c.clear();
        }
        ok = ok && insertRegistryBinaryBackend(database, a, b, c, d, e, ownsUnit, errorMessage);
        if (ok && backendTable.rowSize == ORWA_BACKENDS_ROW_SIZE && !d.empty()) {
            if (registrySupportsLaunchdBackends() && !e.empty()) {
                ok = upsertLaunchdBackendRegistryRecord(database, a.c_str(), e.c_str(), ownsUnit, errorMessage);
            } else if (registrySupportsSystemdBackends()) {
                ok = upsertSystemdBackendRegistryRecord(database, a.c_str(), d.c_str(), errorMessage);
            }
        }
    }

    OrwaTableDescriptor& frontendTable = descriptors[ORWA_TABLE_FRONTENDS];
    for (uint64_t row = 0; ok && row < frontendTable.rowCount; row += 1) {
        const uint64_t rowOffset = frontendTable.offset + row * frontendTable.rowSize;
        const uint8_t* rowBytes = bytes + rowOffset;
        HostedAppEntry entry;
        initHostedAppEntry(entry);
        ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes), readLittleEndianUInt64(rowBytes + 8), a, errorMessage) &&
            readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 16), readLittleEndianUInt64(rowBytes + 24), b, errorMessage) &&
            readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 32), readLittleEndianUInt64(rowBytes + 40), c, errorMessage) &&
            readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 48), readLittleEndianUInt64(rowBytes + 56), e, errorMessage) &&
            readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 64), readLittleEndianUInt64(rowBytes + 72), f, errorMessage);
        d.clear();
        if (ok) {
            if (frontendTable.rowSize == ORWA_LEGACY_FRONTENDS_ROW_SIZE) {
                ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 48), readLittleEndianUInt64(rowBytes + 56), d, errorMessage) &&
                    readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 64), readLittleEndianUInt64(rowBytes + 72), e, errorMessage) &&
                    readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 80), readLittleEndianUInt64(rowBytes + 88), f, errorMessage);
                entry.port = static_cast<int>(readLittleEndianUInt32(rowBytes + 96));
            } else {
                const uint8_t endpointKind = rowBytes[80];
                if (endpointKind == 1u) {
                    entry.port = static_cast<int>(readLittleEndianUInt32(rowBytes + 81));
                } else if (endpointKind == 2u) {
                    ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 81), readLittleEndianUInt64(rowBytes + 89), d, errorMessage);
                    entry.port = 0;
                } else if (endpointKind == 0u) {
                    entry.port = 0;
                } else {
                    ok = setError(errorMessage, "Registry binary frontend endpoint kind is unsupported.");
                }
            }
        }
        if (ok) {
            if (e.rfind("data:", 0) == 0) {
                e.clear();
            }
            ok = assignBuffer(entry.url, a.data(), a.size()) &&
                assignBuffer(entry.name, c.data(), c.size()) &&
                assignBuffer(entry.socketPath, d.data(), d.size()) &&
                assignBuffer(entry.iconPath, e.data(), e.size()) &&
                assignBuffer(entry.list, f.data(), f.size()) &&
                insertFrontendRegistryRecord(database, b.c_str(), entry, errorMessage);
            if (ok && tableCount != ORWA_TABLE_COUNT) {
                ok = insertFrontendLayoutRegistryRecord(database, a.c_str(), f.c_str(), errorMessage);
            }
        }
        freeHostedAppEntry(entry);
        if (!ok && errorMessage.size == 0) {
            setError(errorMessage, "Out of memory.");
        }
    }

    if (tableCount == ORWA_TABLE_COUNT) {
        OrwaTableDescriptor& layoutTable = descriptors[ORWA_TABLE_FRONTEND_LAYOUTS];
        for (uint64_t row = 0; ok && row < layoutTable.rowCount; row += 1) {
            const uint64_t rowOffset = layoutTable.offset + row * layoutTable.rowSize;
            const uint8_t* rowBytes = bytes + rowOffset;
            ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes), readLittleEndianUInt64(rowBytes + 8), a, errorMessage) &&
                readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 16), readLittleEndianUInt64(rowBytes + 24), b, errorMessage) &&
                insertFrontendLayoutRegistryRecord(database, a.c_str(), b.c_str(), errorMessage);
        }
    }

    const size_t logTableIndex = tableCount == ORWA_TABLE_COUNT ? ORWA_TABLE_LOG_FILES : ORWA_TABLE_FRONTEND_LAYOUTS;
    OrwaTableDescriptor& logTable = descriptors[logTableIndex];
    for (uint64_t row = 0; ok && row < logTable.rowCount; row += 1) {
        const uint64_t rowOffset = logTable.offset + row * logTable.rowSize;
        const uint8_t* rowBytes = bytes + rowOffset;
        ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes), readLittleEndianUInt64(rowBytes + 8), a, errorMessage) &&
            readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 16), readLittleEndianUInt64(rowBytes + 24), b, errorMessage) &&
            insertLogFileRegistryRecord(database, b.c_str(), a.c_str(), errorMessage);
    }

    if (tableCount > ORWA_LEGACY_TABLE_SYSTEMD_BACKENDS && registrySupportsSystemdBackends()) {
        OrwaTableDescriptor& systemdTable = descriptors[ORWA_LEGACY_TABLE_SYSTEMD_BACKENDS];
        for (uint64_t row = 0; ok && row < systemdTable.rowCount; row += 1) {
            const uint64_t rowOffset = systemdTable.offset + row * systemdTable.rowSize;
            const uint8_t* rowBytes = bytes + rowOffset;
            ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes), readLittleEndianUInt64(rowBytes + 8), a, errorMessage) &&
                readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 16), readLittleEndianUInt64(rowBytes + 24), b, errorMessage) &&
                upsertSystemdBackendRegistryRecord(database, a.c_str(), b.c_str(), errorMessage);
        }
    }

    if (tableCount > ORWA_LEGACY_TABLE_LAUNCHD_BACKENDS && registrySupportsLaunchdBackends()) {
        OrwaTableDescriptor& launchdTable = descriptors[ORWA_LEGACY_TABLE_LAUNCHD_BACKENDS];
        for (uint64_t row = 0; ok && row < launchdTable.rowCount; row += 1) {
            const uint64_t rowOffset = launchdTable.offset + row * launchdTable.rowSize;
            const uint8_t* rowBytes = bytes + rowOffset;
            ok = readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes), readLittleEndianUInt64(rowBytes + 8), a, errorMessage) &&
                readRegistryBinaryString(file, variableOffset, readLittleEndianUInt64(rowBytes + 16), readLittleEndianUInt64(rowBytes + 24), b, errorMessage) &&
                upsertLaunchdBackendRegistryRecord(database, a.c_str(), b.c_str(), (readLittleEndianUInt32(rowBytes + 32) & 1u) != 0, errorMessage);
        }
    }

    if (ok) {
        ok = sqliteExecWithError(database, "COMMIT;", errorMessage);
    } else {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
    }

    freeBuffer(file);
    return ok;
}

void printTSVField(const char* text) {
    if (!text) {
        return;
    }
    for (const char* cursor = text; *cursor; cursor += 1) {
        if (*cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
            fputc(' ', stdout);
        } else {
            fputc(*cursor, stdout);
        }
    }
}

bool printRegistryQuery(sqlite3* database,
                        const char* sql,
                        const char* backendIdentifier,
                        Buffer& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    if (!sqlitePrepareWithError(database, sql, statement, errorMessage)) {
        return false;
    }
    if (sqlite3_bind_text(statement,
                          1,
                          backendIdentifier && backendIdentifier[0] != '\0' ? backendIdentifier : nullptr,
                          -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(statement);
        return setSqliteError(errorMessage, database, "Failed to bind registry list filter: ");
    }

    const int columnCount = sqlite3_column_count(statement);
    for (int column = 0; column < columnCount; column += 1) {
        if (column != 0) {
            fputc('\t', stdout);
        }
        printTSVField(sqlite3_column_name(statement, column));
    }
    fputc('\n', stdout);

    while (true) {
        const int stepResult = sqlite3_step(statement);
        if (stepResult == SQLITE_DONE) {
            sqlite3_finalize(statement);
            return true;
        }
        if (stepResult != SQLITE_ROW) {
            sqlite3_finalize(statement);
            return setSqliteError(errorMessage, database, "Failed to list registry rows: ");
        }
        for (int column = 0; column < columnCount; column += 1) {
            if (column != 0) {
                fputc('\t', stdout);
            }
            printTSVField(reinterpret_cast<const char*>(sqlite3_column_text(statement, column)));
        }
        fputc('\n', stdout);
    }
}

bool printRegistryList(sqlite3* database,
                       const char* resource,
                       const char* backendIdentifier,
                       bool includeIconValues,
                       Buffer& errorMessage) {
    if (strcmp(resource, "backend") == 0) {
        return printRegistryQuery(database,
                                  includeIconValues ?
                                  "SELECT service_id, display_name, COALESCE(icon_path, '') AS icon_path, COALESCE(unit_name, '') AS unit_name, COALESCE(unit_path, '') AS unit_path, COALESCE(owns_unit, 0) AS owns_unit "
                                  "FROM backends WHERE (?1 IS NULL OR service_id = ?1) ORDER BY service_id;" :
                                  "SELECT service_id, display_name, COALESCE(icon_path, '') AS icon_path, COALESCE(unit_name, '') AS unit_name, COALESCE(unit_path, '') AS unit_path, COALESCE(owns_unit, 0) AS owns_unit "
                                  "FROM backends WHERE (?1 IS NULL OR service_id = ?1) ORDER BY service_id;",
                                  backendIdentifier,
                                  errorMessage);
    }
    if (strcmp(resource, "app") == 0) {
        return printRegistryQuery(database,
                                  includeIconValues ?
                                  "SELECT url, COALESCE(service_id, '') AS service_id, display_name, port, socket_path, COALESCE(icon_path, '') AS icon_path, "
                                  "COALESCE(list, '') AS list "
                                  "FROM frontends WHERE (?1 IS NULL OR service_id = ?1) ORDER BY service_id, COALESCE(list, ''), display_name, url;" :
                                  "SELECT url, COALESCE(service_id, '') AS service_id, display_name, port, socket_path, COALESCE(icon_path, '') AS icon_path, "
                                  "COALESCE(list, '') AS list "
                                  "FROM frontends WHERE (?1 IS NULL OR service_id = ?1) ORDER BY service_id, COALESCE(list, ''), display_name, url;",
                                  backendIdentifier,
                                  errorMessage);
    }
    if (strcmp(resource, "log") == 0) {
        return printRegistryQuery(database,
                                  "SELECT path, service_id FROM log_files WHERE (?1 IS NULL OR service_id = ?1) ORDER BY service_id, path;",
                                  backendIdentifier,
                                  errorMessage);
    }
    if (strcmp(resource, "systemd") == 0) {
        if (!registrySupportsSystemdBackends()) {
            return setError(errorMessage, "systemd registry is unavailable on this platform.");
        }
        return printRegistryQuery(database,
                                  "SELECT service_id, unit_name "
                                  "FROM backends WHERE (?1 IS NULL OR service_id = ?1) AND COALESCE(unit_name, '') != '' ORDER BY service_id;",
                                  backendIdentifier,
                                  errorMessage);
    }
    if (strcmp(resource, "launchd") == 0) {
        if (!registrySupportsLaunchdBackends()) {
            return setError(errorMessage, "launchd registry is unavailable on this platform.");
        }
        return printRegistryQuery(database,
                                  "SELECT service_id, unit_path AS plist_path, owns_unit AS owns_plist "
                                  "FROM backends WHERE (?1 IS NULL OR service_id = ?1) AND COALESCE(unit_path, '') != '' ORDER BY service_id;",
                                  backendIdentifier,
                                  errorMessage);
    }
    return setError(errorMessage, "Unknown registry resource.");
}

bool migrateLegacyRegistryIfNeeded(sqlite3* database, Buffer& errorMessage) {
    Buffer legacyPath;
    initBuffer(legacyPath);
    if (!buildOuterwebappsPath(kLegacyRegistryStorageFileName, legacyPath)) {
        freeBuffer(legacyPath);
        return setError(errorMessage, "Out of memory.");
    }

    struct stat legacyStat {};
    if (stat(legacyPath.data, &legacyStat) != 0) {
        freeBuffer(legacyPath);
        if (errno == ENOENT) {
            return true;
        }
        return setErrnoError(errorMessage, "Failed to inspect legacy registry storage: ");
    }

    RegistryRowArray rows;
    initRegistryRowArray(rows);
    if (!loadLegacyRegistryRows(rows, errorMessage)) {
        freeRegistryRowArray(rows);
        freeBuffer(legacyPath);
        return false;
    }

    if (!sqliteExecWithError(database, "BEGIN IMMEDIATE TRANSACTION;", errorMessage)) {
        freeRegistryRowArray(rows);
        freeBuffer(legacyPath);
        return false;
    }

    bool ok = true;
    for (size_t rowIndex = 0; ok && rowIndex < rows.count; rowIndex += 1) {
        RegistryRow& row = rows.items[rowIndex];
        const char* identifier = row.identifier.data ? row.identifier.data : "";
        if (identifier[0] == '\0') {
            continue;
        }

        Buffer* displayName = findFieldValue(row.fields, static_cast<uint8_t>(RegistryFieldKey::displayName));
        Buffer* systemdUnitName = findFieldValue(row.fields, static_cast<uint8_t>(RegistryFieldKey::systemdUnitName));
        ok = upsertBackendRegistryRecord(database,
                                         identifier,
                                         displayName && displayName->size != 0 ? displayName->data : identifier,
                                         nullptr,
                                         errorMessage) &&
            upsertLogFileRecord(database, identifier, errorMessage) &&
            clearFrontendRegistryRecords(database, identifier, errorMessage);

        if (!ok) {
            break;
        }

        if (registrySupportsSystemdBackends() &&
            systemdUnitName &&
            systemdUnitName->size != 0) {
            ok = upsertSystemdBackendRegistryRecord(database,
                                                    identifier,
                                                    systemdUnitName->data,
                                                    errorMessage);
        }

        if (!ok) {
            break;
        }

        Buffer* hostedAppsField = findFieldValue(row.fields, static_cast<uint8_t>(RegistryFieldKey::hostedApps));
        if (hostedAppsField && hostedAppsField->size != 0) {
            HostedAppArray apps;
            initHostedAppArray(apps);
            if (!deserializeHostedApps(*hostedAppsField, apps, errorMessage)) {
                freeHostedAppArray(apps);
                ok = false;
                break;
            }
            for (size_t appIndex = 0; ok && appIndex < apps.count; appIndex += 1) {
                ok = insertFrontendRegistryRecord(database, identifier, apps.items[appIndex], errorMessage);
            }
            freeHostedAppArray(apps);
        }
    }

    if (ok) {
        ok = sqliteExecWithError(database, "COMMIT;", errorMessage);
    } else {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
    }

    if (ok) {
        unlink(legacyPath.data);
    } else if (errorMessage.size == 0) {
        setError(errorMessage, "Legacy registry migration failed.");
    }

    freeRegistryRowArray(rows);
    freeBuffer(legacyPath);
    return ok;
}

bool openRegistryDatabase(sqlite3*& databaseOut, Buffer& errorMessage) {
    databaseOut = nullptr;

    Buffer rootPath;
    Buffer sqlitePath;
    Buffer binaryPath;
    initBuffer(rootPath);
    initBuffer(sqlitePath);
    initBuffer(binaryPath);
    if (!resolveOuterwebappsHome(rootPath) ||
        !ensureDirectoryExists(rootPath.data, errorMessage) ||
        !buildOuterwebappsPath(kRegistryDatabaseFileName, sqlitePath) ||
        !buildOuterwebappsPath(kRegistryBinaryFileName, binaryPath)) {
        freeBuffer(rootPath);
        freeBuffer(sqlitePath);
        freeBuffer(binaryPath);
        if (errorMessage.size == 0) {
            setError(errorMessage, "Out of memory.");
        }
        return false;
    }

    struct stat binaryStat {};
    struct stat sqliteStat {};
    const bool haveBinary = stat(binaryPath.data, &binaryStat) == 0 && S_ISREG(binaryStat.st_mode);
    const bool haveSqlite = stat(sqlitePath.data, &sqliteStat) == 0 && S_ISREG(sqliteStat.st_mode);
    if (!haveBinary && haveSqlite) {
        sqlite3* migrationDatabase = nullptr;
        const int migrationOpenResult = sqlite3_open_v2(sqlitePath.data,
                                                        &migrationDatabase,
                                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI,
                                                        nullptr);
        if (migrationOpenResult != SQLITE_OK) {
            assignCString(errorMessage, migrationDatabase ? sqlite3_errmsg(migrationDatabase) : "failed to open registry database for migration");
            if (migrationDatabase) {
                sqlite3_close(migrationDatabase);
            }
            freeBuffer(rootPath);
            freeBuffer(sqlitePath);
            freeBuffer(binaryPath);
            return false;
        }
        sqlite3_busy_timeout(migrationDatabase, 5000);
        const bool migrated = ensureRegistrySchema(migrationDatabase, errorMessage) &&
            migrateLegacyRegistryIfNeeded(migrationDatabase, errorMessage) &&
            exportRegistryBinaryFromDatabase(migrationDatabase, errorMessage);
        sqlite3_close(migrationDatabase);
        if (!migrated) {
            freeBuffer(rootPath);
            freeBuffer(sqlitePath);
            freeBuffer(binaryPath);
            return false;
        }
    }

    sqlite3* database = nullptr;
    const int openResult = sqlite3_open_v2(":memory:",
                                           &database,
                                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                           nullptr);
    freeBuffer(rootPath);
    freeBuffer(sqlitePath);
    freeBuffer(binaryPath);
    if (openResult != SQLITE_OK) {
        assignCString(errorMessage, sqlite3_errmsg(database));
        if (database) {
            sqlite3_close(database);
        }
        return false;
    }

    if (!ensureRegistrySchema(database, errorMessage) ||
        !importRegistryBinaryIntoDatabaseIfPresent(database, errorMessage) ||
        !migrateLegacyRegistryIfNeeded(database, errorMessage)) {
        sqlite3_close(database);
        return false;
    }

    databaseOut = database;
    return true;
}

bool parseInt(const char* rawValue, int& value) {
    if (!rawValue || rawValue[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const long parsed = strtol(rawValue, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parseBool(const char* rawValue, bool& value) {
    if (!rawValue) {
        return false;
    }
    if (strcmp(rawValue, "1") == 0 ||
        strcmp(rawValue, "true") == 0 ||
        strcmp(rawValue, "yes") == 0) {
        value = true;
        return true;
    }
    if (strcmp(rawValue, "0") == 0 ||
        strcmp(rawValue, "false") == 0 ||
        strcmp(rawValue, "no") == 0) {
        value = false;
        return true;
    }
    return false;
}

#if defined(__APPLE__)
void postDistributedNotification(const char* notificationName, CFDictionaryRef userInfo = nullptr) {
    if (!notificationName || notificationName[0] == '\0') {
        return;
    }

    CFStringRef cfNotificationName = CFStringCreateWithCString(kCFAllocatorDefault,
                                                               notificationName,
                                                               kCFStringEncodingUTF8);
    if (!cfNotificationName) {
        return;
    }

    CFNotificationCenterPostNotification(CFNotificationCenterGetDistributedCenter(),
                                         cfNotificationName,
                                         nullptr,
                                         userInfo,
                                         true);
    CFRelease(cfNotificationName);
}

void postHostedAppOpenedDistributedNotification(const char* backendIdentifier, int port) {
    if (!backendIdentifier || backendIdentifier[0] == '\0') {
        return;
    }

    CFStringRef notificationName = CFStringCreateWithCString(kCFAllocatorDefault,
                                                             kHostedAppOpenedDistributedNotificationName,
                                                             kCFStringEncodingUTF8);
    CFStringRef backendKey = CFStringCreateWithCString(kCFAllocatorDefault,
                                                       kHostedAppOpenedDistributedNotificationBackendUserInfoKey,
                                                       kCFStringEncodingUTF8);
    CFStringRef backendValue = CFStringCreateWithCString(kCFAllocatorDefault,
                                                         backendIdentifier,
                                                         kCFStringEncodingUTF8);
    if (!notificationName || !backendKey || !backendValue) {
        if (notificationName) {
            CFRelease(notificationName);
        }
        if (backendKey) {
            CFRelease(backendKey);
        }
        if (backendValue) {
            CFRelease(backendValue);
        }
        return;
    }

    const void* keys[2] = {backendKey, nullptr};
    const void* values[2] = {backendValue, nullptr};
    CFIndex count = 1;
    CFStringRef portKey = nullptr;
    CFNumberRef portNumber = nullptr;
    if (port > 0 && port <= 65535) {
        portKey = CFStringCreateWithCString(kCFAllocatorDefault,
                                            kHostedAppOpenedDistributedNotificationPortUserInfoKey,
                                            kCFStringEncodingUTF8);
        if (!portKey) {
            CFRelease(notificationName);
            CFRelease(backendKey);
            CFRelease(backendValue);
            return;
        }
        const int portValue = port;
        portNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &portValue);
        if (!portNumber) {
            CFRelease(portKey);
            CFRelease(notificationName);
            CFRelease(backendKey);
            CFRelease(backendValue);
            return;
        }
        keys[count] = portKey;
        values[count] = portNumber;
        count += 1;
    }

    CFDictionaryRef userInfo = CFDictionaryCreate(kCFAllocatorDefault,
                                                  keys,
                                                  values,
                                                  count,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    if (userInfo) {
        CFNotificationCenterPostNotification(CFNotificationCenterGetDistributedCenter(),
                                             notificationName,
                                             nullptr,
                                             userInfo,
                                             true);
        CFRelease(userInfo);
    }

    if (portNumber) {
        CFRelease(portNumber);
    }
    CFRelease(backendValue);
    if (portKey) {
        CFRelease(portKey);
    }
    CFRelease(backendKey);
    CFRelease(notificationName);
}

void postServiceUIsInvalidatedDistributedNotification() {
    postDistributedNotification(kServiceUIsInvalidatedDistributedNotificationName);
}
#endif

bool requireRegisteredBackend(sqlite3* database,
                              const char* identifier,
                              Buffer& errorMessage) {
    bool exists = false;
    if (!backendRegistryRecordExists(database, identifier, exists, errorMessage)) {
        return false;
    }
    if (exists) {
        return true;
    }
    return setError(errorMessage, "Backend not registered. Run outerctl backend upsert first.");
}

void printUsage() {
    fprintf(stderr,
            "Usage:\n"
            "  outerctl backend list [--backend <identifier>] [--icons]\n"
            "  outerctl backend upsert --backend <identifier> [--name <name>] [--icon-path <path>]\n"
            "  outerctl backend remove --backend <identifier>\n"
            "  outerctl systemd list [--backend <identifier>]\n"
            "  outerctl systemd set --backend <identifier> --unit <name>\n"
            "  outerctl systemd clear --backend <identifier>\n"
            "  outerctl launchd list [--backend <identifier>]\n"
            "  outerctl launchd set --backend <identifier> --plist <path> [--owns-plist <true|false>]\n"
            "  outerctl launchd clear --backend <identifier>\n"
            "  outerctl log list [--backend <identifier>]\n"
            "  outerctl log add --backend <identifier> --path <path>\n"
            "  outerctl log remove --backend <identifier> --path <path>\n"
            "  outerctl log clear --backend <identifier>\n"
            "  outerctl app list [--backend <identifier>] [--icons]\n"
            "  outerctl app add --backend <identifier> (--port <port> | --socket-path <path>) --name <name> [--url <url>] [--icon-path <path>] [--list <name>]\n"
            "  outerctl app remove --backend <identifier> (--port <port> | --socket-path <path>)\n"
            "  outerctl app clear --backend <identifier>\n"
            "  outerctl opener list [--backend <identifier>] [--extension <ext> | --kind <kind>] [--file <path>]\n"
            "  outerctl opener add --backend <identifier> (--extension <ext> | --kind <kind>) --socket-path <path> --name <name> [--url-template <template>] [--rank <rank>]\n"
            "  outerctl opener remove --backend <identifier> (--extension <ext> | --kind <kind>)\n"
            "  outerctl opener clear --backend <identifier>\n");
}

} // namespace

int main(int argc, char* argv[]) {
    int apiExitStatus = 1;
    Buffer apiError;
    initBuffer(apiError);
    if (tryOuterctlApi(argc, argv, apiExitStatus, apiError)) {
        freeBuffer(apiError);
        return apiExitStatus;
    }
    fprintf(stderr, "%s\n", apiError.data ? apiError.data : "Failed to call outershelld API.");
    freeBuffer(apiError);
    return 1;
}
