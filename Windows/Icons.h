#ifndef OUTER_WINDOWS_ICONS_H
#define OUTER_WINDOWS_ICONS_H
#include "Runtime.h"
bool icons_init(const char *state);
void icons_describe(const char *const *identity, size_t count, StringBuilder *image, char token[33]);
int icons_save(const char *token, const void *bytes, size_t length);
#endif
