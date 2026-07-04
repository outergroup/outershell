#import "OuterframeAccessibility.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} OFAXWriter;

typedef struct {
    const OFAccessibilityNode *node;
    uint32_t parent_index;
} OFAXFlatNode;

typedef struct {
    OFAXFlatNode *nodes;
    size_t count;
    size_t capacity;
} OFAXFlatNodeList;

enum {
    OFAXFormatVersion = 1,
    OFAXHeaderSize = 16,
    OFAXNodeRecordSize = 74,
    OFAXMaximumNodeCount = 100000,
    OFAXStringLabelFlag = 1 << 0,
    OFAXStringValueFlag = 1 << 1,
    OFAXStringHintFlag = 1 << 2,
    OFAXRowCountFlag = 1 << 3,
    OFAXColumnCountFlag = 1 << 4,
    OFAXEnabledFlag = 1 << 5,
};

static bool OFAXReserve(OFAXWriter *writer, size_t additional) {
    if (additional > SIZE_MAX - writer->length) return false;
    size_t needed = writer->length + additional;
    if (needed <= writer->capacity) return true;
    size_t capacity = writer->capacity ? writer->capacity : 128;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    uint8_t *bytes = realloc(writer->bytes, capacity);
    if (!bytes) return false;
    writer->bytes = bytes;
    writer->capacity = capacity;
    return true;
}

static bool OFAXWriteBytes(OFAXWriter *writer, const void *bytes, size_t length) {
    if (!OFAXReserve(writer, length)) return false;
    memcpy(writer->bytes + writer->length, bytes, length);
    writer->length += length;
    return true;
}

static bool OFAXWriteU8(OFAXWriter *writer, uint8_t value) {
    return OFAXWriteBytes(writer, &value, sizeof(value));
}

static bool OFAXWriteU32(OFAXWriter *writer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xff),
        (uint8_t)((value >> 8) & 0xff),
        (uint8_t)((value >> 16) & 0xff),
        (uint8_t)((value >> 24) & 0xff),
    };
    return OFAXWriteBytes(writer, bytes, sizeof(bytes));
}

static bool OFAXWriteU64(OFAXWriter *writer, uint64_t value) {
    uint8_t bytes[8] = {
        (uint8_t)(value & 0xff),
        (uint8_t)((value >> 8) & 0xff),
        (uint8_t)((value >> 16) & 0xff),
        (uint8_t)((value >> 24) & 0xff),
        (uint8_t)((value >> 32) & 0xff),
        (uint8_t)((value >> 40) & 0xff),
        (uint8_t)((value >> 48) & 0xff),
        (uint8_t)((value >> 56) & 0xff),
    };
    return OFAXWriteBytes(writer, bytes, sizeof(bytes));
}

static bool OFAXWriteF64(OFAXWriter *writer, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return OFAXWriteU64(writer, bits);
}

static bool OFAXWriteI32(OFAXWriter *writer, int32_t value) {
    return OFAXWriteU32(writer, (uint32_t)value);
}

static void OFAXFreeWriter(OFAXWriter *writer) {
    free(writer->bytes);
    writer->bytes = NULL;
    writer->length = 0;
    writer->capacity = 0;
}

static void OFAXFreeFlatNodeList(OFAXFlatNodeList *list) {
    free(list->nodes);
    list->nodes = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool OFAXAppendFlatNode(OFAXFlatNodeList *list, const OFAccessibilityNode *node, uint32_t parent_index) {
    if (!node || list->count >= OFAXMaximumNodeCount) return true;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        if (capacity > OFAXMaximumNodeCount) capacity = OFAXMaximumNodeCount;
        OFAXFlatNode *nodes = realloc(list->nodes, capacity * sizeof(*nodes));
        if (!nodes) return false;
        list->nodes = nodes;
        list->capacity = capacity;
    }

    uint32_t current_index = (uint32_t)list->count;
    list->nodes[list->count++] = (OFAXFlatNode){
        .node = node,
        .parent_index = parent_index,
    };

    for (size_t i = 0; i < node->child_count && list->count < OFAXMaximumNodeCount; i++) {
        if (!OFAXAppendFlatNode(list, &node->children[i], current_index)) {
            return false;
        }
    }
    return true;
}

static bool OFAXAppendStringReference(OFAXWriter *string_writer,
                                      const char *string,
                                      size_t variable_data_offset,
                                      uint32_t *out_offset,
                                      uint32_t *out_length) {
    *out_offset = 0;
    *out_length = 0;
    if (!string) return true;

    size_t length = strlen(string);
    if (length > UINT32_MAX || string_writer->length > SIZE_MAX - variable_data_offset) {
        return false;
    }
    size_t offset = variable_data_offset + string_writer->length;
    if (offset > UINT32_MAX) {
        return false;
    }
    if (!OFAXWriteBytes(string_writer, string, length)) {
        return false;
    }
    *out_offset = (uint32_t)offset;
    *out_length = (uint32_t)length;
    return true;
}

static bool OFAXEncodeFlatNode(OFAXWriter *record_writer,
                               OFAXWriter *string_writer,
                               const OFAXFlatNode *flat_node,
                               size_t variable_data_offset) {
    const OFAccessibilityNode *node = flat_node->node;
    uint8_t flags = node->enabled ? OFAXEnabledFlag : 0;
    uint32_t label_offset = 0;
    uint32_t label_length = 0;
    uint32_t value_offset = 0;
    uint32_t value_length = 0;
    uint32_t hint_offset = 0;
    uint32_t hint_length = 0;

    if (!OFAXAppendStringReference(string_writer, node->label, variable_data_offset, &label_offset, &label_length) ||
        !OFAXAppendStringReference(string_writer, node->value, variable_data_offset, &value_offset, &value_length) ||
        !OFAXAppendStringReference(string_writer, node->hint, variable_data_offset, &hint_offset, &hint_length)) {
        return false;
    }

    if (node->label) flags |= OFAXStringLabelFlag;
    if (node->value) flags |= OFAXStringValueFlag;
    if (node->hint) flags |= OFAXStringHintFlag;
    if (node->has_row_count) flags |= OFAXRowCountFlag;
    if (node->has_column_count) flags |= OFAXColumnCountFlag;

    return OFAXWriteU32(record_writer, node->identifier) &&
           OFAXWriteU32(record_writer, flat_node->parent_index) &&
           OFAXWriteF64(record_writer, node->frame.origin.x) &&
           OFAXWriteF64(record_writer, node->frame.origin.y) &&
           OFAXWriteF64(record_writer, node->frame.size.width) &&
           OFAXWriteF64(record_writer, node->frame.size.height) &&
           OFAXWriteU32(record_writer, label_offset) &&
           OFAXWriteU32(record_writer, label_length) &&
           OFAXWriteU32(record_writer, value_offset) &&
           OFAXWriteU32(record_writer, value_length) &&
           OFAXWriteU32(record_writer, hint_offset) &&
           OFAXWriteU32(record_writer, hint_length) &&
           OFAXWriteI32(record_writer, node->has_row_count ? node->row_count : 0) &&
           OFAXWriteI32(record_writer, node->has_column_count ? node->column_count : 0) &&
           OFAXWriteU8(record_writer, node->role) &&
           OFAXWriteU8(record_writer, flags);
}

bool OFAccessibilitySnapshotEncode(const OFAccessibilitySnapshot *snapshot, OFBuffer *out_data) {
    if (!snapshot || !out_data) return false;
    OFAXFlatNodeList flat_nodes = {0};
    for (size_t i = 0; i < snapshot->root_count; i++) {
        if (!OFAXAppendFlatNode(&flat_nodes, &snapshot->root_nodes[i], UINT32_MAX)) {
            OFAXFreeFlatNodeList(&flat_nodes);
            return false;
        }
    }

    if (flat_nodes.count > (SIZE_MAX - OFAXHeaderSize) / OFAXNodeRecordSize) {
        OFAXFreeFlatNodeList(&flat_nodes);
        return false;
    }
    size_t node_records_offset = OFAXHeaderSize;
    size_t node_records_size = flat_nodes.count * OFAXNodeRecordSize;
    size_t variable_data_offset = node_records_offset + node_records_size;
    if (node_records_offset > UINT32_MAX || node_records_size > UINT32_MAX || variable_data_offset > UINT32_MAX) {
        OFAXFreeFlatNodeList(&flat_nodes);
        return false;
    }

    OFAXWriter record_writer = {0};
    OFAXWriter string_writer = {0};
    bool ok = true;
    for (size_t i = 0; ok && i < flat_nodes.count; i++) {
        ok = OFAXEncodeFlatNode(&record_writer, &string_writer, &flat_nodes.nodes[i], variable_data_offset);
    }
    if (!ok || record_writer.length != node_records_size) {
        OFAXFreeWriter(&record_writer);
        OFAXFreeWriter(&string_writer);
        OFAXFreeFlatNodeList(&flat_nodes);
        return false;
    }

    OFAXWriter writer = {0};
    ok = OFAXWriteU32(&writer, OFAXFormatVersion) &&
         OFAXWriteU32(&writer, OFAXNodeRecordSize) &&
         OFAXWriteU32(&writer, (uint32_t)node_records_offset) &&
         OFAXWriteU32(&writer, (uint32_t)node_records_size) &&
         OFAXWriteBytes(&writer, record_writer.bytes, record_writer.length) &&
         OFAXWriteBytes(&writer, string_writer.bytes, string_writer.length);
    OFAXFreeWriter(&record_writer);
    OFAXFreeWriter(&string_writer);
    OFAXFreeFlatNodeList(&flat_nodes);
    if (!ok) {
        OFAXFreeWriter(&writer);
        return false;
    }
    out_data->bytes = writer.bytes;
    out_data->length = writer.length;
    return true;
}

bool OFAccessibilityNotImplementedSnapshot(const char *message, OFBuffer *out_data) {
    OFAccessibilityNode child = {
        .identifier = 1,
        .role = OFAccessibilityRoleStaticText,
        .frame = CGRectZero,
        .label = message ? message : "Accessibility not implemented",
        .enabled = true,
    };
    OFAccessibilityNode root = {
        .identifier = 0,
        .role = OFAccessibilityRoleContainer,
        .frame = CGRectZero,
        .enabled = true,
        .children = &child,
        .child_count = 1,
    };
    OFAccessibilitySnapshot snapshot = {
        .root_nodes = &root,
        .root_count = 1,
    };
    return OFAccessibilitySnapshotEncode(&snapshot, out_data);
}
