#define main outershelld_main
#include "../outershelld/outershelld.c"
#undef main
#include <assert.h>

int main(void) {
    const size_t count = 6000;
    char *query = malloc(count * 3 + 32);
    assert(query);
    strcpy(query, "command=");
    for (size_t i = 0; i < count; i++) memcpy(query + 8 + i * 3, "%20", 3);
    strcpy(query + 8 + count * 3, "&name=Example");
    char command[8192];
    assert(query_value(query, "command", command, sizeof(command)));
    assert(strlen(command) == count);
    for (size_t i = 0; i < count; i++) assert(command[i] == ' ');
    char name[32];
    assert(query_value(query, "name", name, sizeof(name)));
    assert(strcmp(name, "Example") == 0);
    assert(!query_value(query, "missing", name, sizeof(name)));
    free(query);
    puts("PASS: long encoded commands retain all bytes and adjacent fields");
}
