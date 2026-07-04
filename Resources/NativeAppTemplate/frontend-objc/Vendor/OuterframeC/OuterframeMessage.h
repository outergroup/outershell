#ifndef OUTERFRAME_MESSAGE_H
#define OUTERFRAME_MESSAGE_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OFContentSocketHeaderLength = sizeof(uint32_t),
    OFContentSocketMessageTypeLength = sizeof(uint16_t)
};

typedef struct {
    const uint8_t *bytes;
    size_t length;
} OFDataView;

typedef struct {
    const char *bytes;
    size_t length;
} OFStringView;

typedef struct {
    uint8_t bytes[16];
} OFUUID;

typedef struct {
    uint8_t *bytes;
    size_t length;
} OFBuffer;

static inline uint16_t OFReadUInt16LE(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static inline uint32_t OFReadUInt32LE(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

typedef uint8_t OFInitArgKind;
enum {
    OFInitArgKindData = 1,
    OFInitArgKindContentSize = 2,
    OFInitArgKindAppearance = 3,
    OFInitArgKindProxy = 4,
    OFInitArgKindProxyAuth = 5,
    OFInitArgKindURL = 6,
    OFInitArgKindBundleURL = 7,
    OFInitArgKindWindowIsActive = 8,
    OFInitArgKindHistoryEntryID = 9,
};

typedef uint16_t OFBrowserMessageKind;
enum {
    OFBrowserMessageInitializeContent = 1000,
    OFBrowserMessageResizeContent = 1001,
    OFBrowserMessageShutdown = 1002,
    OFBrowserMessageDisplayLinkFired = 1003,
    OFBrowserMessageDisplayLinkCallbackRegistered = 1004,
    OFBrowserMessageSystemAppearanceUpdate = 1005,
    OFBrowserMessageWindowActiveUpdate = 1006,
    OFBrowserMessageViewFocusChanged = 1007,
    OFBrowserMessageMouseDown = 1008,
    OFBrowserMessageMouseDragged = 1009,
    OFBrowserMessageMouseUp = 1010,
    OFBrowserMessageMouseMoved = 1011,
    OFBrowserMessageRightMouseDown = 1012,
    OFBrowserMessageRightMouseUp = 1013,
    OFBrowserMessageScrollWheelEvent = 1014,
    OFBrowserMessageKeyDown = 1015,
    OFBrowserMessageKeyUp = 1016,
    OFBrowserMessageMagnification = 1017,
    OFBrowserMessageMagnificationEnded = 1018,
    OFBrowserMessageQuickLook = 1019,
    OFBrowserMessageTextInput = 1020,
    OFBrowserMessageSetMarkedText = 1021,
    OFBrowserMessageUnmarkText = 1022,
    OFBrowserMessageTextInputFocus = 1023,
    OFBrowserMessageTextCommand = 1024,
    OFBrowserMessageSetCursorPosition = 1025,
    OFBrowserMessageSelectionToPasteboardCopyRequest = 1026,
    OFBrowserMessagePasteboardContentPasted = 1027,
    OFBrowserMessageAccessibilitySnapshotRequest = 1028,
    OFBrowserMessageHistoryEntryAccepted = 1029,
    OFBrowserMessageHistoryEntryRejected = 1030,
    OFBrowserMessageHistoryTraversal = 1031,
    OFBrowserMessageHistoryContextUpdate = 1032,
    OFBrowserMessageContextMenuItemSelected = 1033,
    OFBrowserMessagePasteboardAccessResponse = 1034,
    OFBrowserMessagePasteboardContentDropped = 1035,
    OFBrowserMessageSelectionToPasteboardCutRequest = 1037,
    OFBrowserMessagePasteboardDropHitTestRequest = 1038,
    OFBrowserMessageFilePromiseWriteRequest = 1039,
    OFBrowserMessageEditCommandValidationRequest = 1040,
    OFBrowserMessageCopySelectedPasteboardRequest = OFBrowserMessageSelectionToPasteboardCopyRequest,
    OFBrowserMessagePasteboardContentDelivered = OFBrowserMessagePasteboardContentPasted,
};

typedef uint16_t OFContentMessageKind;
enum {
    OFContentMessageStartDisplayLink = 2000,
    OFContentMessageStopDisplayLink = 2001,
    OFContentMessageCursorUpdate = 2002,
    OFContentMessageInputModeUpdate = 2003,
    OFContentMessageTextInputGeometryUpdate = 2004,
    OFContentMessageShowContextMenu = 2005,
    OFContentMessageShowDefinition = 2006,
    OFContentMessageHapticFeedback = 2007,
    OFContentMessageSelectionToPasteboardResponse = 2008,
    OFContentMessageEditCommandValidationResponse = 2009,
    OFContentMessageAccessibilitySnapshotResponse = 2010,
    OFContentMessageAccessibilityTreeChanged = 2011,
    OFContentMessageOpenNewWindow = 2012,
    OFContentMessageHistoryPushEntry = 2013,
    OFContentMessageHistoryReplaceEntry = 2014,
    OFContentMessageHistoryGo = 2015,
    OFContentMessageShowContextMenuItems = 2016,
    OFContentMessagePasteboardAccessRequest = 2017,
    OFContentMessageBeginDraggingPasteboardItems = 2018,
    OFContentMessageSetPasteboardDropBehaviorUniform = 2021,
    OFContentMessageSetAcceptedPasteboardPasteTypes = 2022,
    OFContentMessagePasteboardDropHitTestResponse = 2023,
    OFContentMessageSetPasteboardDropBehaviorHitTest = 2024,
    OFContentMessageReleaseDroppedFileAccess = 2026,
    OFContentMessageFilePromiseWriteResponse = 2027,
    OFContentMessageSetTitle = 2030,
    OFContentMessageSetIcon = 2031,
    OFContentMessageTextCursorUpdate = OFContentMessageTextInputGeometryUpdate,
    OFContentMessageCopySelectedPasteboardResponse = OFContentMessageSelectionToPasteboardResponse,
};

typedef uint32_t OFEditCommandSet;
enum {
    OFEditCommandCopy = 1u << 0,
    OFEditCommandCut = 1u << 1,
    OFEditCommandPaste = 1u << 2,
    OFEditCommandSelectAll = 1u << 3,
    OFEditCommandStandard = OFEditCommandCopy | OFEditCommandCut | OFEditCommandPaste | OFEditCommandSelectAll,
};

typedef uint8_t OFCursorType;
enum {
    OFCursorTypeArrow = 0,
    OFCursorTypeIBeam = 1,
    OFCursorTypeCrosshair = 2,
    OFCursorTypeOpenHand = 3,
    OFCursorTypeClosedHand = 4,
    OFCursorTypePointingHand = 5,
    OFCursorTypeResizeLeft = 6,
    OFCursorTypeResizeRight = 7,
    OFCursorTypeResizeLeftRight = 8,
    OFCursorTypeResizeUp = 9,
    OFCursorTypeResizeDown = 10,
    OFCursorTypeResizeUpDown = 11,
};

typedef uint8_t OFContentInputMode;
enum {
    OFContentInputModeNone = 0,
    OFContentInputModeTextInput = 1 << 0,
    OFContentInputModeRawKeys = 1 << 1,
};

typedef uint8_t OFHapticFeedbackStyle;
enum {
    OFHapticFeedbackStyleGeneric = 0,
    OFHapticFeedbackStyleAlignment = 1,
    OFHapticFeedbackStyleLevelChange = 2,
};

typedef struct {
    bool present;
    OFStringView host;
    uint16_t port;
    bool has_username;
    OFStringView username;
    bool has_password;
    OFStringView password;
} OFInitializeContentProxy;

typedef struct {
    bool has_data;
    OFDataView data;
    bool has_content_size;
    CGSize content_size;
    bool has_appearance_archive;
    OFDataView appearance_archive;
    OFInitializeContentProxy proxy;
    bool has_url;
    OFStringView url;
    bool has_bundle_url;
    OFStringView bundle_url;
    bool has_window_is_active;
    bool window_is_active;
    bool has_history_entry_id;
    OFUUID history_entry_id;
} OFInitializeContent;

typedef struct {
    OFStringView type_identifier;
    OFDataView data;
} OFPasteboardItemView;

typedef struct {
    OFUUID field_id;
    CGRect rect;
} OFTextInputGeometry;

typedef struct {
    uint16_t key_code;
    OFStringView characters;
    OFStringView characters_ignoring_modifiers;
    uint64_t modifier_flags;
    bool is_a_repeat;
} OFKeyEvent;

typedef struct {
    OFBrowserMessageKind kind;
    union {
        OFInitializeContent initialize;
        struct { uint64_t frame_number; double target_timestamp; } display_link_fired;
        struct { OFUUID callback_id; OFUUID browser_callback_id; } display_link_callback_registered;
        CGSize resize;
        struct { double x; double y; uint64_t modifier_flags; uint32_t click_count; } mouse;
        struct { double x; double y; double delta_x; double delta_y; uint64_t modifier_flags; uint32_t phase; uint32_t momentum_phase; bool has_precise_scrolling_deltas; } scroll;
        OFKeyEvent key;
        struct { uint32_t surface_id; double magnification; double x; double y; double scroll_x; double scroll_y; } magnification;
        struct { double x; double y; } point;
        struct { OFStringView text; bool has_replacement_range; uint64_t replacement_location; uint64_t replacement_length; } text_input;
        struct { OFStringView text; uint64_t selected_location; uint64_t selected_length; bool has_replacement_range; uint64_t replacement_location; uint64_t replacement_length; } marked_text;
        struct { OFUUID field_id; bool has_focus; } text_focus;
        struct { OFStringView command; } text_command;
        struct { OFUUID field_id; uint64_t position; bool modify_selection; } cursor_position;
        struct { OFDataView appearance_archive; } appearance;
        struct { bool value; } boolean_update;
        struct { OFUUID request_id; } request;
        struct { OFUUID request_id; OFEditCommandSet commands; } edit_validation;
        struct { OFPasteboardItemView *items; size_t count; } pasteboard;
        struct { OFUUID entry_id; OFStringView url; OFStringView error_message; uint32_t length; bool can_go_back; bool can_go_forward; } history;
    } as;
} OFBrowserMessage;

bool OFBrowserMessageDecode(const uint8_t *message, size_t message_length, OFBrowserMessage *out_message);
void OFBrowserMessageFree(OFBrowserMessage *message);

bool OFEncodeFrame(uint16_t type, OFDataView payload, OFBuffer *out_frame);
bool OFEncodeCursorUpdate(OFCursorType cursor_type, OFBuffer *out_frame);
bool OFEncodeInputModeUpdate(OFContentInputMode input_mode, OFBuffer *out_frame);
bool OFEncodeShowContextMenu(OFDataView attributed_text_rtf, double location_x, double location_y, OFBuffer *out_frame);
bool OFEncodeShowDefinition(OFDataView attributed_text_rtf, double location_x, double location_y, OFBuffer *out_frame);
bool OFEncodeSetTitle(const char *title_or_null, OFBuffer *out_frame);
bool OFEncodeSetIconBundleResource(const char *path_or_null, OFBuffer *out_frame);
bool OFEncodeAccessibilitySnapshotResponse(OFUUID request_id, const uint8_t *snapshot_or_null, size_t snapshot_length, OFBuffer *out_frame);
bool OFEncodeAccessibilityTreeChanged(uint8_t notification_mask, OFBuffer *out_frame);
bool OFEncodeHapticFeedback(OFHapticFeedbackStyle style, OFBuffer *out_frame);
bool OFEncodeStartDisplayLink(OFUUID callback_id, OFBuffer *out_frame);
bool OFEncodeStopDisplayLink(OFUUID browser_callback_id, OFBuffer *out_frame);
bool OFEncodeCopySelectedPasteboardResponse(OFUUID request_id, const OFPasteboardItemView *items, size_t item_count, OFBuffer *out_frame);
bool OFEncodeEditCommandValidationResponse(OFUUID request_id, OFEditCommandSet enabled_commands, OFBuffer *out_frame);
bool OFEncodeTextInputGeometryUpdate(const OFTextInputGeometry *geometry_or_null, OFBuffer *out_frame);
bool OFEncodeOpenNewWindow(const char *url, const char *display_string_or_null, bool has_preferred_size, CGSize preferred_size, OFBuffer *out_frame);
bool OFEncodeHistoryEntry(uint16_t message_type, OFUUID entry_id, const char *url_or_null, OFBuffer *out_frame);
bool OFEncodeHistoryGo(int32_t delta, OFBuffer *out_frame);
void OFBufferFree(OFBuffer *buffer);

#ifdef __cplusplus
}
#endif

#endif
