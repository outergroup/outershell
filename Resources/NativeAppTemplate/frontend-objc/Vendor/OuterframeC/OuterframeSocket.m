#import "OuterframeSocket.h"
#import "OuterframeMessage.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} OFSocketBuffer;

struct OFContentSocket {
    int32_t fd;
    bool stopped;
    bool write_source_resumed;
    dispatch_queue_t queue;
    dispatch_source_t read_source;
    dispatch_source_t write_source;
    OFContentSocketCallbacks callbacks;
    void *context;
    OFSocketBuffer incoming;
    OFSocketBuffer pending_write;
};

static bool OFSocketBufferReserve(OFSocketBuffer *buffer, size_t additional) {
    if (additional > SIZE_MAX - buffer->length) return false;
    size_t needed = buffer->length + additional;
    if (needed <= buffer->capacity) return true;

    size_t capacity = buffer->capacity ? buffer->capacity : 4096;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    uint8_t *bytes = realloc(buffer->bytes, capacity);
    if (!bytes) return false;
    buffer->bytes = bytes;
    buffer->capacity = capacity;
    return true;
}

static bool OFSocketBufferAppend(OFSocketBuffer *buffer, const uint8_t *bytes, size_t length) {
    if (!OFSocketBufferReserve(buffer, length)) return false;
    memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length += length;
    return true;
}

static void OFSocketBufferRemovePrefix(OFSocketBuffer *buffer, size_t length) {
    if (length >= buffer->length) {
        buffer->length = 0;
        return;
    }
    memmove(buffer->bytes, buffer->bytes + length, buffer->length - length);
    buffer->length -= length;
}

static void OFSocketBufferFree(OFSocketBuffer *buffer) {
    free(buffer->bytes);
    buffer->bytes = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

static void OFSocketSetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags != -1) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void OFSocketNotifyClosed(OFContentSocket *socket) {
    if (!socket->callbacks.closed) return;
    OFContentSocketCallbacks callbacks = socket->callbacks;
    void *context = socket->context;
    dispatch_async(dispatch_get_main_queue(), ^{
        callbacks.closed(socket, context);
    });
}

static void OFSocketStopOnQueue(OFContentSocket *socket, bool notify_closed) {
    if (socket->stopped) return;
    socket->stopped = true;

    if (socket->read_source) {
        dispatch_source_cancel(socket->read_source);
        socket->read_source = NULL;
    }

    if (socket->write_source) {
        if (!socket->write_source_resumed) {
            dispatch_resume(socket->write_source);
            socket->write_source_resumed = true;
        }
        dispatch_source_cancel(socket->write_source);
        socket->write_source = NULL;
    }

    if (socket->fd >= 0) {
        close(socket->fd);
        socket->fd = -1;
    }

    OFSocketBufferFree(&socket->incoming);
    OFSocketBufferFree(&socket->pending_write);

    if (notify_closed) {
        OFSocketNotifyClosed(socket);
    }
}

static void OFSocketProcessIncoming(OFContentSocket *socket) {
    while (socket->incoming.length >= OFContentSocketHeaderLength) {
        uint8_t *bytes = socket->incoming.bytes;
        uint32_t message_length = OFReadUInt32LE(bytes);
        size_t total_length = OFContentSocketHeaderLength + (size_t)message_length;
        if (socket->incoming.length < total_length) break;

        uint8_t *message_copy = NULL;
        if (message_length > 0) {
            message_copy = malloc(message_length);
            if (!message_copy) {
                OFSocketStopOnQueue(socket, true);
                return;
            }
            memcpy(message_copy, bytes + OFContentSocketHeaderLength, message_length);
        }
        OFSocketBufferRemovePrefix(&socket->incoming, total_length);

        if (socket->callbacks.message) {
            OFContentSocketCallbacks callbacks = socket->callbacks;
            void *context = socket->context;
            dispatch_async(dispatch_get_main_queue(), ^{
                callbacks.message(socket, message_copy, message_length, context);
                free(message_copy);
            });
        } else {
            free(message_copy);
        }
    }
}

static void OFSocketHandleReadable(OFContentSocket *socket) {
    if (socket->stopped || socket->fd < 0) return;

    uint8_t bytes[4096];
    while (true) {
        ssize_t count = read(socket->fd, bytes, sizeof(bytes));
        if (count > 0) {
            if (!OFSocketBufferAppend(&socket->incoming, bytes, (size_t)count)) {
                OFSocketStopOnQueue(socket, true);
                return;
            }
            OFSocketProcessIncoming(socket);
        } else if (count == 0) {
            OFSocketStopOnQueue(socket, true);
            return;
        } else {
            if (errno == EWOULDBLOCK || errno == EAGAIN) return;
            OFSocketStopOnQueue(socket, true);
            return;
        }
    }
}

static void OFSocketSuspendWriteSourceIfNeeded(OFContentSocket *socket) {
    if (socket->write_source && socket->write_source_resumed) {
        dispatch_suspend(socket->write_source);
        socket->write_source_resumed = false;
    }
}

static void OFSocketResumeWriteSourceIfNeeded(OFContentSocket *socket) {
    if (socket->write_source && !socket->write_source_resumed) {
        dispatch_resume(socket->write_source);
        socket->write_source_resumed = true;
    }
}

static void OFSocketDrainPendingWrites(OFContentSocket *socket) {
    if (socket->stopped || socket->fd < 0) return;

    while (socket->pending_write.length > 0) {
        ssize_t written = write(socket->fd, socket->pending_write.bytes, socket->pending_write.length);
        if (written > 0) {
            OFSocketBufferRemovePrefix(&socket->pending_write, (size_t)written);
            continue;
        }
        if (written == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            return;
        }
        OFSocketStopOnQueue(socket, true);
        return;
    }

    OFSocketSuspendWriteSourceIfNeeded(socket);
}

static void OFSocketWriteImmediatelyOrEnqueue(OFContentSocket *socket, const uint8_t *bytes, size_t length) {
    if (socket->stopped || socket->fd < 0 || !bytes || length == 0) return;

    if (socket->pending_write.length > 0) {
        if (OFSocketBufferAppend(&socket->pending_write, bytes, length)) {
            OFSocketResumeWriteSourceIfNeeded(socket);
        }
        return;
    }

    ssize_t written = write(socket->fd, bytes, length);
    if (written == (ssize_t)length) {
        return;
    }

    if (written > 0) {
        size_t written_length = (size_t)written;
        if (OFSocketBufferAppend(&socket->pending_write, bytes + written_length, length - written_length)) {
            OFSocketResumeWriteSourceIfNeeded(socket);
        }
        return;
    }

    if (written == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        if (OFSocketBufferAppend(&socket->pending_write, bytes, length)) {
            OFSocketResumeWriteSourceIfNeeded(socket);
        }
        return;
    }

    OFSocketStopOnQueue(socket, true);
}

OFContentSocket *OFContentSocketCreate(int32_t fd, OFContentSocketCallbacks callbacks, void *context) {
    OFContentSocket *socket = calloc(1, sizeof(*socket));
    if (!socket) return NULL;

    socket->fd = fd;
    socket->callbacks = callbacks;
    socket->context = context;
    socket->queue = dispatch_queue_create("dev.outergroup.outerframec.socket", DISPATCH_QUEUE_SERIAL);

    OFSocketSetNonBlocking(fd);
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof(one));

    socket->read_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, socket->queue);
    if (!socket->read_source) {
        OFContentSocketDestroy(socket);
        return NULL;
    }

    socket->write_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, (uintptr_t)fd, 0, socket->queue);
    if (!socket->write_source) {
        OFContentSocketDestroy(socket);
        return NULL;
    }

    __block OFContentSocket *block_socket = socket;
    dispatch_source_set_event_handler(socket->read_source, ^{
        OFSocketHandleReadable(block_socket);
    });
    dispatch_source_set_cancel_handler(socket->read_source, ^{
    });

    dispatch_source_set_event_handler(socket->write_source, ^{
        OFSocketDrainPendingWrites(block_socket);
    });
    dispatch_source_set_cancel_handler(socket->write_source, ^{
    });

    dispatch_resume(socket->read_source);
    socket->write_source_resumed = false;
    return socket;
}

void OFContentSocketDestroy(OFContentSocket *socket) {
    if (!socket) return;

    dispatch_sync(socket->queue, ^{
        OFSocketStopOnQueue(socket, false);
    });

    free(socket);
}

void OFContentSocketSend(OFContentSocket *socket, const uint8_t *bytes, size_t length) {
    if (!socket || !bytes || length == 0) return;

    uint8_t *copy = malloc(length);
    if (!copy) return;
    memcpy(copy, bytes, length);

    dispatch_async(socket->queue, ^{
        if (!socket->stopped && OFSocketBufferAppend(&socket->pending_write, copy, length)) {
            OFSocketResumeWriteSourceIfNeeded(socket);
            OFSocketDrainPendingWrites(socket);
        }
        free(copy);
    });
}

void OFContentSocketSendBlocking(OFContentSocket *socket, const uint8_t *bytes, size_t length) {
    if (!socket || !bytes || length == 0) return;

    uint8_t *copy = malloc(length);
    if (!copy) return;
    memcpy(copy, bytes, length);

    dispatch_sync(socket->queue, ^{
        OFSocketWriteImmediatelyOrEnqueue(socket, copy, length);
        OFSocketDrainPendingWrites(socket);
    });

    free(copy);
}
