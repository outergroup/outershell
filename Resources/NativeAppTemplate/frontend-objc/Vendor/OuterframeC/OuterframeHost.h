#ifndef OUTERFRAME_HOST_H
#define OUTERFRAME_HOST_H

#include "OuterframeAccessibility.h"
#include "OuterframeMessage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OFHost OFHost;

typedef void (*OFHostMessageCallback)(OFHost *host, const OFBrowserMessage *message, void *context);
typedef void (*OFHostDisconnectCallback)(OFHost *host, void *context);
typedef void (*OFHostDisplayLinkCallback)(OFHost *host, double target_timestamp, void *context);
typedef struct {
    OFHostMessageCallback message;
    OFHostDisconnectCallback disconnected;
} OFHostCallbacks;

OFHost *OFHostCreate(int32_t socket_fd, OFHostCallbacks callbacks, void *context);
void OFHostDestroy(OFHost *host);

void OFHostSetMessageCallback(OFHost *host, OFHostMessageCallback callback, void *context);

void OFHostConfigureFromInitialize(OFHost *host, const OFInitializeContent *initialize);
const char *OFHostURL(OFHost *host);
const char *OFHostBundleURL(OFHost *host);
OFUUID OFHostCurrentHistoryEntryID(OFHost *host);
uint32_t OFHostHistoryLength(OFHost *host);
bool OFHostCanGoBackInHistory(OFHost *host);
bool OFHostCanGoForwardInHistory(OFHost *host);

void OFHostSetCursor(OFHost *host, OFCursorType cursor_type);
void OFHostSetInputMode(OFHost *host, OFContentInputMode input_mode);
void OFHostSetTitle(OFHost *host, const char *title_or_null);
void OFHostSetIconBundleResource(OFHost *host, const char *path_or_null);
void OFHostUpdatePageMetadata(OFHost *host, const char *title_or_null, const uint8_t *icon_png_or_null, size_t icon_png_length, uint32_t icon_width, uint32_t icon_height);
void OFHostUpdateStartPageMetadata(OFHost *host, const char *title_or_null, const uint8_t *icon_png_or_null, size_t icon_png_length, uint32_t icon_width, uint32_t icon_height);
void OFHostShowContextMenu(OFHost *host, OFDataView attributed_text_rtf, double location_x, double location_y);
void OFHostShowDefinition(OFHost *host, OFDataView attributed_text_rtf, double location_x, double location_y);
void OFHostSetPasteboardCapabilities(OFHost *host, bool can_copy, bool can_cut, const char *const *pasteboard_types, size_t type_count);
void OFHostSendCopySelectedPasteboardResponse(OFHost *host, OFUUID request_id, const OFPasteboardItemView *items, size_t item_count);
void OFHostSendEditCommandValidationResponse(OFHost *host, OFUUID request_id, OFEditCommandSet enabled_commands);
void OFHostSendAccessibilitySnapshotResponse(OFHost *host, OFUUID request_id, const uint8_t *snapshot_or_null, size_t snapshot_length);
void OFHostSendAccessibilityTreeChanged(OFHost *host, OFAccessibilityNotification notification_mask);
void OFHostPerformHapticFeedback(OFHost *host, OFHapticFeedbackStyle style);
OFUUID OFHostPushHistoryEntry(OFHost *host, const char *url_or_null);
OFUUID OFHostReplaceHistoryEntry(OFHost *host, const char *url_or_null);
void OFHostGoInHistory(OFHost *host, int32_t delta);
void OFHostGoBackInHistory(OFHost *host);
void OFHostGoForwardInHistory(OFHost *host);
OFUUID OFHostRegisterDisplayLinkCallback(OFHost *host, OFHostDisplayLinkCallback callback, void *context);
void OFHostStopDisplayLinkCallback(OFHost *host, OFUUID callback_id);
#ifdef __cplusplus
}
#endif

#endif
