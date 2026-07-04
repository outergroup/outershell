#ifndef OUTERFRAME_SOCKET_H
#define OUTERFRAME_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OFContentSocket OFContentSocket;

typedef struct {
    void (*message)(OFContentSocket *socket, const uint8_t *message, size_t message_length, void *context);
    void (*closed)(OFContentSocket *socket, void *context);
} OFContentSocketCallbacks;

OFContentSocket *OFContentSocketCreate(int32_t fd, OFContentSocketCallbacks callbacks, void *context);
void OFContentSocketDestroy(OFContentSocket *socket);
void OFContentSocketSend(OFContentSocket *socket, const uint8_t *bytes, size_t length);
void OFContentSocketSendBlocking(OFContentSocket *socket, const uint8_t *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
