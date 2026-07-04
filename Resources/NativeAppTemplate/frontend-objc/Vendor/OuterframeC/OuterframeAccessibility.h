#ifndef OUTERFRAME_ACCESSIBILITY_H
#define OUTERFRAME_ACCESSIBILITY_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "OuterframeMessage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t OFAccessibilityRole;
enum {
    OFAccessibilityRoleContainer = 0,
    OFAccessibilityRoleStaticText = 1,
    OFAccessibilityRoleButton = 2,
    OFAccessibilityRoleImage = 3,
    OFAccessibilityRoleTable = 4,
    OFAccessibilityRoleRow = 5,
    OFAccessibilityRoleCell = 6,
    OFAccessibilityRoleTextField = 7,
};

typedef uint8_t OFAccessibilityNotification;
enum {
    OFAccessibilityNotificationLayoutChanged = 1 << 0,
    OFAccessibilityNotificationSelectedChildrenChanged = 1 << 1,
    OFAccessibilityNotificationFocusedElementChanged = 1 << 2,
};

typedef struct OFAccessibilityNode {
    uint32_t identifier;
    OFAccessibilityRole role;
    CGRect frame;
    const char *label;
    const char *value;
    const char *hint;
    bool has_row_count;
    int32_t row_count;
    bool has_column_count;
    int32_t column_count;
    bool enabled;
    const struct OFAccessibilityNode *children;
    size_t child_count;
} OFAccessibilityNode;

typedef struct {
    const OFAccessibilityNode *root_nodes;
    size_t root_count;
} OFAccessibilitySnapshot;

bool OFAccessibilitySnapshotEncode(const OFAccessibilitySnapshot *snapshot, OFBuffer *out_data);
bool OFAccessibilityNotImplementedSnapshot(const char *message, OFBuffer *out_data);

#ifdef __cplusplus
}
#endif

#endif
