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

namespace {

constexpr uint32_t kOuterctlApiMaxFrameLength = 65536;

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

bool appendPathComponent(Buffer &path, const char *component) {
    if (!component || !component[0]) return true;
    if (path.size > 0 && path.data[path.size - 1] != '/') {
        if (!appendCString(path, "/")) return false;
    }
    return appendCString(path, component);
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

void defaultOuterctlApiSocketPath(char *out, size_t outSize) {
    const char *envPath = getenv("OUTERSHELLD_API_SOCKET");
    if (envPath && envPath[0]) {
        snprintf(out, outSize, "%s", envPath);
        return;
    }
#if defined(__APPLE__)
    const char *tmp = getenv("DARWIN_USER_TEMP_DIR");
    if (!tmp || !tmp[0]) tmp = getenv("TMPDIR");
    if (tmp && tmp[0]) {
        snprintf(out, outSize, "%s%soutershelld-api", tmp, tmp[strlen(tmp) - 1] == '/' ? "" : "/");
        return;
    }
    snprintf(out, outSize, "/tmp/outershelld-api-%d", static_cast<int>(getuid()));
#else
    if (geteuid() == 0) {
        snprintf(out, outSize, "/run/outershelld-api");
        return;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(out, outSize, "%s/outershelld-api", runtime);
        return;
    }
    snprintf(out, outSize, "/run/user/%d/outershelld-api", static_cast<int>(getuid()));
#endif
}

bool outerShellHome(Buffer &path) {
    const char *override = getenv("OUTERSHELL_HOME");
    if (override && override[0]) return appendCString(path, override);
#if defined(__APPLE__)
    const char *home = getenv("HOME");
    return home && home[0] &&
        appendCString(path, home) &&
        appendPathComponent(path, "Library/Application Support/outershell");
#else
    if (geteuid() == 0) {
        return appendCString(path, "/var/lib/outershell");
    }
    const char *stateHome = getenv("XDG_STATE_HOME");
    if (stateHome && stateHome[0]) {
        return appendCString(path, stateHome) && appendPathComponent(path, "outershell");
    }
    const char *home = getenv("HOME");
    return home && home[0] &&
        appendCString(path, home) &&
        appendPathComponent(path, ".local/state/outershell");
#endif
}

bool outerctlApiRegistryPath(Buffer &path) {
    const char *registry = getenv("OUTERSHELL_REGISTRY");
    if (!registry || !registry[0]) registry = getenv("BACKENDS_REGISTRY_DB");
    if (registry && registry[0]) return appendCString(path, registry);
    return outerShellHome(path) && appendPathComponent(path, "registry.orwa");
}

bool tryOuterctlApi(int argc, char *argv[], int &exitStatus, Buffer &errorMessage) {
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
    Buffer registryPath;
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
    freeBuffer(registryPath);
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
    if (responseLength < 22 || responseLength > kOuterctlApiMaxFrameLength) {
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
    if (readLittleEndianUInt16(responseBytes) != 2) {
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
    if (tryOuterctlApi(argc, argv, apiExitStatus, apiError)) {
        freeBuffer(apiError);
        return apiExitStatus;
    }
    fprintf(stderr, "%s\n", apiError.data ? apiError.data : "Failed to call outershelld API.");
    freeBuffer(apiError);
    return 1;
}
