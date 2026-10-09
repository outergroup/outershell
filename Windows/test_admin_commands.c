#define wmain outershell_main
#include "OuterShell.c"
#undef wmain
static void admin_form_field(StringBuilder *body, const char *key, const char *value) {
    if (body->length) sb_append(body, "&");
    sb_append(body, key); sb_append(body, "=");
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        char escaped[4]; snprintf(escaped, sizeof(escaped), "%%%02X", *p); sb_append(body, escaped);
    }
}

static void fixture_request(Connection *connection, void *context) {
    HttpRequest request = {0};
    if (http_read(connection, &request)) admin_commands_request(context, connection, &request);
    http_free(&request);
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--cleanup-fixture")) {
        ShellHost admin = {0};
        if (!admin_host(&admin, false) || !commands_load(&admin)) return 1;
        CommandApp *app = command_find(&admin, "org.outershell.command.admin-regression-fixture");
        char error[256];
        bool ok = !app || command_control(&admin, app, "uninstall", error, sizeof(error));
        if (!ok) puts(error);
        admin_host_free(&admin);
        return ok ? 0 : 1;
    }
    if (argc > 1) return outershell_main(argc, argv);
    ShellHost host = {0};
    InitializeCriticalSection(&host.gate);
    InitializeConditionVariable(&host.changed);
    HttpRequest r = {0};
    admin_form_field(&r.body, "scope", "system");
    admin_form_field(&r.body, "name", "Administrator regression fixture");
    admin_form_field(&r.body, "identifier", "admin-regression-fixture");
    admin_form_field(&r.body, "command", "whoami /groups\r\necho ADMIN_FIXTURE_READY\r\nC:\\Windows\\System32\\ping.exe -n 300 127.0.0.1 >nul");
    admin_form_field(&r.body, "frontendTransport", "port");
    admin_form_field(&r.body, "port", "48631");
    char *name = win_pipe("org.outershell.admin-test");
    PipeServer *server = pipe_server_start(name, fixture_request, &host);
    wchar_t *wide = win_wide(name);
    HANDLE client = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (!server || client == INVALID_HANDLE_VALUE) return 1;
    StringBuilder wire = {0}; char header[256];
    snprintf(header, sizeof(header), "POST /api/admin-commands HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\n", r.body.length);
    sb_append(&wire, header); sb_append_n(&wire, r.body.data, r.body.length);
    DWORD transferred = 0;
    if (!WriteFile(client, wire.data, (DWORD)wire.length, &transferred, NULL)) return 1;
    StringBuilder received = {0}; char buffer[4096];
    while (ReadFile(client, buffer, sizeof(buffer), &transferred, NULL) && transferred) sb_append_n(&received, buffer, transferred);
    CloseHandle(client); pipe_server_stop(server);
    char *body = received.data ? strstr(received.data, "\r\n\r\n") : NULL;
    if (!body || !strstr(received.data, "200 OK")) { puts(received.data ? received.data : "No HTTP result"); return 1; }
    body += 4;
    StringBuilder response = {0}; sb_append_n(&response, body, received.length - (size_t)(body - received.data));
    free(name); free(wide); free(wire.data); free(received.data);
    char *message = payload_string(response.data, response.length, 4);
    puts(message ? message : "No response");
    bool ok = response.length >= 4 && read_uint32_le((unsigned char *)response.data) == 1;
    free(message); free(response.data); free(r.body.data);
    DeleteCriticalSection(&host.gate);
    return ok ? 0 : 1;
}
