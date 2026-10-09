#include "Runtime.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <process.h>

void *win_alloc(size_t size) {
    void *result = calloc(1, size ? size : 1);
    if (!result) {
        fputs("Out of memory.\n", stderr);
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    }
    return result;
}
char *win_strdup(const char *value) {
    size_t n = strlen(value) + 1;
    char *r = win_alloc(n);
    memcpy(r, value, n);
    return r;
}
wchar_t *win_wide(const char *value) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (!n)
        return NULL;
    wchar_t *r = win_alloc((size_t)n * sizeof(*r));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, r, n)) {
        free(r);
        return NULL;
    }
    return r;
}
char *win_utf8(const wchar_t *value) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    if (!n)
        return NULL;
    char *r = win_alloc((size_t)n);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, r, n, NULL, NULL)) {
        free(r);
        return NULL;
    }
    return r;
}
char *win_join(const char *base, const char *name) {
    size_t n = strlen(base) + strlen(name) + 2;
    char *r = win_alloc(n);
    snprintf(r, n, "%s/%s", base, name);
    return r;
}
char *win_folder(int folder) {
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, folder, NULL, SHGFP_TYPE_CURRENT, path)))
        return NULL;
    return win_utf8(path);
}
char *win_executable_directory(void) {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(NULL, path, 32768);
    if (!n || n == 32768)
        return NULL;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
        return NULL;
    *slash = 0;
    return win_utf8(path);
}
char *win_sid(void) {
    HANDLE token;
    DWORD length = 0;
    char *result = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &length);
    TOKEN_USER *user = win_alloc(length);
    LPWSTR sid = NULL;
    if (GetTokenInformation(token, TokenUser, user, length, &length) &&
        ConvertSidToStringSidW(user->User.Sid, &sid))
        result = win_utf8(sid);
    LocalFree(sid);
    free(user);
    CloseHandle(token);
    return result;
}
char *win_pipe(const char *app) {
    char *sid = win_sid();
    if (!sid)
        return NULL;
    size_t n = strlen(app) + strlen(sid) + 11;
    char *r = win_alloc(n);
    snprintf(r, n, "\\\\.\\pipe\\%s.%s", app, sid);
    free(sid);
    for (char *p = r; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return r;
}
static PSECURITY_DESCRIPTOR security_descriptor(bool directory) {
    char *sid = win_sid();
    if (!sid)
        return NULL;
    char text[512];
    snprintf(text, sizeof(text),
             directory ? "O:%sD:P(A;OICI;FA;;;%s)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
                       : "O:%sD:P(A;;GA;;;%s)(A;;GA;;;SY)",
             sid, sid);
    free(sid);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(text, SDDL_REVISION_1, &sd, NULL))
        return NULL;
    return sd;
}
bool win_secure_directory(const char *path) {
    wchar_t *wide = win_wide(path);
    PSECURITY_DESCRIPTOR sd = security_descriptor(true);
    if (!wide || !sd) {
        free(wide);
        LocalFree(sd);
        return false;
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    bool ok = CreateDirectoryW(wide, &sa) || GetLastError() == ERROR_ALREADY_EXISTS;
    HANDLE h = ok ? CreateFileW(wide, READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                  : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE)
        ok = false;
    else {
        BY_HANDLE_FILE_INFORMATION info;
        PACL acl = NULL;
        PSID owner = NULL;
        BOOL present, defaulted;
        ok = GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
             !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
             GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted) &&
             GetSecurityDescriptorOwner(sd, &owner, &defaulted) &&
             SetSecurityInfo(h, SE_FILE_OBJECT,
                             OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                 PROTECTED_DACL_SECURITY_INFORMATION,
                             owner, NULL, acl, NULL) == ERROR_SUCCESS;
        CloseHandle(h);
    }
    free(wide);
    LocalFree(sd);
    return ok;
}
bool win_atomic_write(const char *path, const void *data, size_t length) {
    wchar_t *wide = win_wide(path);
    if (!wide)
        return false;
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        free(wide);
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    wchar_t *temp = win_alloc((wcslen(wide) + 64) * sizeof(wchar_t));
    static volatile LONG serial;
    swprintf(temp, wcslen(wide) + 64, L"%s.%lu.%lu.tmp", wide, GetCurrentProcessId(),
             (unsigned long)InterlockedIncrement(&serial));
    HANDLE h = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = h != INVALID_HANDLE_VALUE;
    size_t offset = 0;
    while (ok && offset < length) {
        DWORD count;
        DWORD n = (DWORD)((length - offset) > 65536 ? 65536 : length - offset);
        ok = WriteFile(h, (const char *)data + offset, n, &count, NULL) && count;
        if (ok)
            offset += count;
    }
    if (ok)
        ok = FlushFileBuffers(h) != 0;
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    if (ok)
        ok = MoveFileExW(temp, wide, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileW(temp);
    free(temp);
    free(wide);
    return ok;
}
bool win_read_file(const char *path, size_t limit, StringBuilder *result) {
    wchar_t *wide = win_wide(path);
    if (!wide)
        return false;
    HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && (uint64_t)size.QuadPart <= limit;
    char buffer[65536];
    uint64_t remaining = ok ? (uint64_t)size.QuadPart : 0;
    while (ok && remaining) {
        DWORD count, n = remaining > sizeof(buffer) ? sizeof(buffer) : (DWORD)remaining;
        ok = ReadFile(h, buffer, n, &count, NULL) && count && sb_append_n(result, buffer, count);
        if (ok)
            remaining -= count;
    }
    CloseHandle(h);
    return ok;
}
static bool allow_pipes(const char *const *paths, size_t count) {
    char *base = win_folder(CSIDL_LOCAL_APPDATA);
    if (!base)
        return false;
    char *dir = win_join(base, "OuterSSH"), *path = win_join(dir, "http-pipe.allow");
    free(base);
    bool ok = win_secure_directory(dir);
    StringBuilder content = {0};
    wchar_t *wide = win_wide(path);
    DWORD attr = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    if (ok && attr != INVALID_FILE_ATTRIBUTES)
        ok = !(attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
             win_read_file(path, HTTP_BODY_LIMIT, &content);
    for (size_t i = 0; ok && i < count; i++) {
        bool found = false;
        size_t n = strlen(paths[i]);
        for (size_t at = 0; at < content.length;) {
            size_t end = at;
            while (end < content.length && content.data[end] != '\n')
                end++;
            size_t trim = end;
            if (trim > at && content.data[trim - 1] == '\r')
                trim--;
            if (trim - at == n && !_strnicmp(content.data + at, paths[i], n))
                found = true;
            at = end + 1;
        }
        if (!found) {
            if (content.length && content.data[content.length - 1] != '\n')
                ok = sb_append(&content, "\n");
            ok = ok && sb_append(&content, paths[i]) && sb_append(&content, "\n");
        }
    }
    if (ok)
        ok = win_atomic_write(path, content.data, content.length);
    free(content.data);
    free(dir);
    free(path);
    return ok;
}

bool win_allow_pipes(const char *const *paths, size_t count) {
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    bool ok = allow_pipes(paths, count);
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

struct PipeServer {
    HANDLE stop, listener, slots, first;
    wchar_t *name;
    PSECURITY_DESCRIPTOR sd;
    PipeHandler handler;
    void *context;
};
typedef struct {
    PipeServer *server;
    HANDLE pipe;
} PipeWork;
static bool pipe_io(Connection *c, void *data, DWORD size, DWORD *count, bool write) {
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent)
        return false;
    BOOL ok = write ? WriteFile(c->pipe, data, size, count, &ov) : ReadFile(c->pipe, data, size, count, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        HANDLE events[] = {c->stop, ov.hEvent, c->cancel};
        DWORD wait = WaitForMultipleObjects(c->cancel ? 3 : 2, events, FALSE, 35000);
        if (wait != WAIT_OBJECT_0 + 1)
            CancelIoEx(c->pipe, &ov);
        ok = GetOverlappedResult(c->pipe, &ov, count, TRUE) && wait == WAIT_OBJECT_0 + 1;
    }
    CloseHandle(ov.hEvent);
    return ok && *count;
}
bool connection_read_some(Connection *c, void *data, size_t capacity, size_t *length) {
    if (c->offset < c->length) {
        *length = c->length - c->offset;
        if (*length > capacity) *length = capacity;
        memcpy(data, c->buffer + c->offset, *length);
        c->offset += *length;
        return true;
    }
    DWORD count = 0;
    if (!capacity || !pipe_io(c, data, (DWORD)(capacity > 65536 ? 65536 : capacity), &count, false)) return false;
    *length = count;
    return true;
}
bool connection_read(Connection *c, void *data, size_t length) {
    char *out = data;
    while (length) {
        if (c->offset == c->length) {
            DWORD n = 0;
            if (!pipe_io(c, c->buffer, sizeof(c->buffer), &n, false))
                return false;
            c->offset = 0;
            c->length = n;
        }
        size_t n = c->length - c->offset;
        if (n > length)
            n = length;
        memcpy(out, c->buffer + c->offset, n);
        c->offset += n;
        out += n;
        length -= n;
    }
    return true;
}
bool connection_write(Connection *c, const void *data, size_t length) {
    const char *p = data;
    while (length) {
        DWORD n = 0;
        DWORD chunk = length > 65536 ? 65536 : (DWORD)length;
        if (!pipe_io(c, (void *)p, chunk, &n, true))
            return false;
        p += n;
        length -= n;
    }
    return true;
}
static unsigned __stdcall pipe_worker(void *argument) {
    PipeWork *work = argument;
    PipeServer *s = work->server;
    Connection c = {0};
    c.pipe = work->pipe;
    c.stop = s->stop;
    free(work);
    s->handler(&c, s->context);
    CloseHandle(c.pipe);
    ReleaseSemaphore(s->slots, 1, NULL);
    return 0;
}
static HANDLE new_pipe(PipeServer *s, bool first) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), s->sd, FALSE};
    return CreateNamedPipeW(
        s->name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 255, 65536, 65536, 0,
        &sa);
}
static unsigned __stdcall pipe_listener(void *argument) {
    PipeServer *s = argument;
    HANDLE pipe = s->first;
    while (WaitForSingleObject(s->stop, 0) != WAIT_OBJECT_0) {
        OVERLAPPED ov = {0};
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent)
            break;
        bool connected = ConnectNamedPipe(pipe, &ov) != 0;
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
            connected = true;
        else if (error == ERROR_IO_PENDING) {
            HANDLE events[] = {s->stop, ov.hEvent};
            DWORD n;
            DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0 + 1)
                CancelIoEx(pipe, &ov);
            connected = GetOverlappedResult(pipe, &ov, &n, TRUE) && wait == WAIT_OBJECT_0 + 1;
        }
        CloseHandle(ov.hEvent);
        if (!connected)
            break;
        HANDLE next = new_pipe(s, false);
        if (next == INVALID_HANDLE_VALUE)
            break;
        if (WaitForSingleObject(s->slots, 0) == WAIT_OBJECT_0) {
            PipeWork *work = win_alloc(sizeof(*work));
            work->server = s;
            work->pipe = pipe;
            HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, pipe_worker, work, 0, NULL);
            if (thread)
                CloseHandle(thread);
            else {
                free(work);
                CloseHandle(pipe);
                ReleaseSemaphore(s->slots, 1, NULL);
            }
        } else
            CloseHandle(pipe);
        pipe = next;
    }
    CloseHandle(pipe);
    return 0;
}
PipeServer *pipe_server_start(const char *name, PipeHandler handler, void *context) {
    PipeServer *s = win_alloc(sizeof(*s));
    s->name = win_wide(name);
    s->sd = security_descriptor(false);
    s->handler = handler;
    s->context = context;
    s->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->slots = CreateSemaphoreW(NULL, 64, 64, NULL);
    s->first = INVALID_HANDLE_VALUE;
    if (s->name && s->sd && s->stop && s->slots)
        s->first = new_pipe(s, true);
    if (s->first != INVALID_HANDLE_VALUE)
        s->listener = (HANDLE)_beginthreadex(NULL, 0, pipe_listener, s, 0, NULL);
    if (!s->listener) {
        if (s->first != INVALID_HANDLE_VALUE)
            CloseHandle(s->first);
        pipe_server_stop(s);
        return NULL;
    }
    return s;
}
void pipe_server_stop(PipeServer *s) {
    if (!s)
        return;
    if (s->stop)
        SetEvent(s->stop);
    if (s->listener) {
        WaitForSingleObject(s->listener, INFINITE);
        CloseHandle(s->listener);
        for (unsigned i = 0; i < 64; i++)
            WaitForSingleObject(s->slots, INFINITE);
    }
    if (s->stop)
        CloseHandle(s->stop);
    if (s->slots)
        CloseHandle(s->slots);
    LocalFree(s->sd);
    free(s->name);
    free(s);
}

StringBuilder payload(size_t length) {
    StringBuilder p = {0};
    if (!binary_append_zero(&p, length))
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}
void payload_text(StringBuilder *p, size_t offset, const char *text) {
    if (!binary_append_string_ref_at(p, offset, text ? text : ""))
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
}
void payload_data(StringBuilder *p, size_t offset, const void *data, size_t length) {
    if (!binary_append_data_ref_at(p, offset, data, length))
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
}
void payload_u32(StringBuilder *p, size_t offset, uint32_t v) {
    if (!binary_write_u32_at(p, offset, v))
        ExitProcess(ERROR_INVALID_DATA);
}
void payload_u64(StringBuilder *p, size_t offset, uint64_t v) {
    if (offset > p->length || p->length - offset < 8)
        ExitProcess(ERROR_INVALID_DATA);
    write_uint64_le((unsigned char *)p->data + offset, v);
}
uint64_t payload_read64(const void *bytes) {
    const unsigned char *p = bytes;
    uint64_t n = 0;
    for (int i = 7; i >= 0; i--)
        n = (n << 8) | p[i];
    return n;
}
char *payload_string(const void *bytes, size_t length, size_t ref) {
    const unsigned char *p = bytes;
    if (ref > length || length - ref < 8)
        return NULL;
    uint32_t offset = read_uint32_le(p + ref), n = read_uint32_le(p + ref + 4);
    if (offset > length || n > length - offset || memchr(p + offset, 0, n))
        return NULL;
    char *r = win_alloc((size_t)n + 1);
    memcpy(r, p + offset, n);
    wchar_t *wide = win_wide(r);
    if (!wide) {
        free(r);
        return NULL;
    }
    free(wide);
    return r;
}
static bool http_line(Connection *c, StringBuilder *line, size_t limit) {
    line->length = 0;
    for (;;) {
        char ch;
        if (!connection_read(c, &ch, 1))
            return false;
        if (ch == '\r') {
            if (!connection_read(c, &ch, 1) || ch != '\n')
                return false;
            return true;
        }
        if (!ch || ch == '\n' || line->length >= limit || !sb_append_n(line, &ch, 1))
            return false;
    }
}
static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static char *url_decode(const char *s, size_t length, bool plus) {
    char *r = win_alloc(length + 1);
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%') {
            if (i + 2 >= length || hex_value(s[i + 1]) < 0 || hex_value(s[i + 2]) < 0) {
                free(r);
                return NULL;
            }
            c = (unsigned char)(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
            i += 2;
        } else if (c == '+' && plus)
            c = ' ';
        if (!c || c < 32 || c == 127) {
            free(r);
            return NULL;
        }
        r[out++] = (char)c;
    }
    wchar_t *wide = win_wide(r);
    if (!wide) {
        free(r);
        return NULL;
    }
    free(wide);
    return r;
}
char *http_query(const HttpRequest *r, const char *name) {
    const char *p = r->query ? r->query : "";
    size_t n = strlen(name);
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end)
            end = p + strlen(p);
        const char *equal = memchr(p, '=', (size_t)(end - p));
        if (equal && (size_t)(equal - p) == n && !memcmp(p, name, n))
            return url_decode(equal + 1, (size_t)(end - equal - 1), true);
        p = *end ? end + 1 : end;
    }
    return NULL;
}
static bool http_body_read(Connection *c, StringBuilder *body, size_t length) {
    if (length > HTTP_BODY_LIMIT - body->length || !sb_reserve(body, length))
        return false;
    if (!connection_read(c, body->data + body->length, length))
        return false;
    body->length += length;
    body->data[body->length] = 0;
    return true;
}
bool http_read(Connection *c, HttpRequest *r) {
    StringBuilder line = {0}, names = {0};
    bool ok = false;
    size_t total = 0, content_length = 0;
    bool has_length = false, chunked = false, expect = false;
    if (!http_line(c, &line, 8192) || !line.length)
        goto done;
    char *space = strchr(line.data, ' ');
    if (!space)
        goto done;
    *space++ = 0;
    char *version = strchr(space, ' ');
    if (!version)
        goto done;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))
        goto done;
    if (*space != '/' || strchr(space, '#'))
        goto done;
    r->method = win_strdup(line.data);
    char *query = strchr(space, '?');
    if (query)
        *query++ = 0;
    r->query = win_strdup(query ? query : "");
    r->path = url_decode(space, strlen(space), false);
    if (!r->path)
        goto done;
    for (;;) {
        if (!http_line(c, &line, 65536) || (total += line.length + 2) > 65536)
            goto done;
        if (!line.length)
            break;
        char *colon = strchr(line.data, ':');
        if (!colon || colon == line.data)
            goto done;
        *colon++ = 0;
        for (char *p = line.data; *p; p++) {
            if (!isalnum((unsigned char)*p) && !strchr("!#$%&'*+-.^_`|~", *p))
                goto done;
            *p = (char)tolower((unsigned char)*p);
        }
        for (size_t at = 0; at < names.length; at += strlen(names.data + at) + 1)
            if (!strcmp(names.data + at, line.data))
                goto done;
        if (!sb_append_n(&names, line.data, strlen(line.data) + 1))
            goto done;
        while (*colon == ' ' || *colon == '\t')
            colon++;
        char *end = colon + strlen(colon);
        while (end > colon && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        for (char *p = colon; *p; p++)
            if ((unsigned char)*p < 32 && *p != '\t')
                goto done;
        if (!strcmp(line.data, "outerframe-accept")) {
            const char *media = "application/vnd.outerframe";
            for (const char *p = colon; *p; p++) {
                if (!_strnicmp(p, media, strlen(media))) {
                    r->accepts_outerframe = true;
                    break;
                }
            }
        }
        if (!strcmp(line.data, "content-length")) {
            if (!*colon)
                goto done;
            has_length = true;
            for (char *p = colon; *p; p++) {
                if (*p < '0' || *p > '9' || content_length > HTTP_BODY_LIMIT / 10)
                    goto done;
                content_length = content_length * 10 + (size_t)(*p - '0');
                if (content_length > HTTP_BODY_LIMIT)
                    goto done;
            }
        } else if (!strcmp(line.data, "transfer-encoding")) {
            if (_stricmp(colon, "chunked"))
                goto done;
            chunked = true;
        } else if (!strcmp(line.data, "host"))
            r->host = win_strdup(colon);
        else if (!strcmp(line.data, "origin"))
            r->origin = win_strdup(colon);
        else if (!strcmp(line.data, "expect")) {
            if (_stricmp(colon, "100-continue"))
                goto done;
            expect = true;
        }
    }
    if (has_length && chunked)
        goto done;
    if (expect && !connection_write(c, "HTTP/1.1 100 Continue\r\n\r\n", 25))
        goto done;
    if (chunked) {
        for (;;) {
            if (!http_line(c, &line, 1024) || !line.length)
                goto done;
            size_t n = 0;
            char *p = line.data;
            while (*p && *p != ';') {
                int h = hex_value((unsigned char)*p++);
                if (h < 0 || n > HTTP_BODY_LIMIT / 16)
                    goto done;
                n = n * 16 + (size_t)h;
                if (n > HTTP_BODY_LIMIT)
                    goto done;
            }
            if (p == line.data)
                goto done;
            if (!n) {
                size_t trailers = 0;
                do {
                    if (!http_line(c, &line, 8192) || (trailers += line.length + 2) > 65536)
                        goto done;
                } while (line.length);
                break;
            }
            char crlf[2];
            if (!http_body_read(c, &r->body, n) || !connection_read(c, crlf, 2) || memcmp(crlf, "\r\n", 2))
                goto done;
        }
    } else if (!http_body_read(c, &r->body, content_length))
        goto done;
    ok = true;
done:
    free(line.data);
    free(names.data);
    if (!ok)
        http_text(c, r, 400, "Invalid HTTP request or body size.");
    return ok;
}
void http_free(HttpRequest *r) {
    free(r->method);
    free(r->path);
    free(r->query);
    free(r->host);
    free(r->origin);
    free(r->headers.data);
    free(r->body.data);
    memset(r, 0, sizeof(*r));
}
bool http_authorize(Connection *c, const HttpRequest *r) {
    if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD") && strcmp(r->method, "POST")) {
        http_text(c, r, 405, "Unsupported method.");
        return false;
    }
    if (!strcmp(r->method, "POST") && r->origin) {
        bool ok = r->host && !strncmp(r->origin, "http://", 7) && !_stricmp(r->origin + 7, r->host);
        if (!ok) {
            http_text(c, r, 403, "Origin is not allowed.");
            return false;
        }
    }
    return true;
}
const char *http_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";
    static const char *types[][2] = {{".html", "text/html; charset=utf-8"},
                                     {".js", "text/javascript; charset=utf-8"},
                                     {".css", "text/css; charset=utf-8"},
                                     {".json", "application/json"},
                                     {".svg", "image/svg+xml"},
                                     {".png", "image/png"},
                                     {".jpg", "image/jpeg"},
                                     {".jpeg", "image/jpeg"},
                                     {".gif", "image/gif"},
                                     {".webp", "image/webp"},
                                     {".ico", "image/x-icon"},
                                     {".txt", "text/plain; charset=utf-8"},
                                     {".md", "text/plain; charset=utf-8"},
                                     {".pdf", "application/pdf"},
                                     {".mp4", "video/mp4"},
                                     {".webm", "video/webm"},
                                     {".mp3", "audio/mpeg"},
                                     {".wav", "audio/wav"},
                                     {".woff2", "font/woff2"}};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (!_stricmp(ext, types[i][0]))
            return types[i][1];
    return "application/octet-stream";
}
bool http_header(Connection *c, int status, const char *type, uint64_t length, const char *disposition) {
    if (c->responded)
        return false;
    c->responded = true;
    const char *reason = status == 200   ? "OK"
                         : status == 400 ? "Bad Request"
                         : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found"
                         : status == 405 ? "Method Not Allowed"
                         : status == 409 ? "Conflict"
                         : status == 413 ? "Content Too Large"
                         : status == 501 ? "Not Implemented"
                                         : "Internal Server Error";
    char header[1024];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nConnection: "
                     "close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n",
                     status, reason, type, (unsigned long long)length);
    if (n < 0 || (size_t)n >= sizeof(header) || !connection_write(c, header, (size_t)n))
        return false;
    if (disposition &&
        (!connection_write(c, "Content-Disposition: ", 21) ||
         !connection_write(c, disposition, strlen(disposition)) || !connection_write(c, "\r\n", 2)))
        return false;
    return connection_write(c, "\r\n", 2);
}
void http_send(Connection *c, const HttpRequest *r, int status, const char *type, const void *body,
               size_t length) {
    if (http_header(c, status, type, length, NULL) && (!r->method || strcmp(r->method, "HEAD")))
        connection_write(c, body, length);
}
void http_text(Connection *c, const HttpRequest *r, int status, const char *message) {
    http_send(c, r, status, "text/plain; charset=utf-8", message, strlen(message));
}
void http_windows_error(Connection *c, const HttpRequest *r, DWORD error) {
    char message[128];
    snprintf(message, sizeof(message), "Windows operation failed (%lu).", error);
    http_text(c, r,
              error == ERROR_ACCESS_DENIED                                     ? 403
              : error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 404
                                                                               : 400,
              message);
}
void http_file(Connection *c, const HttpRequest *r, const char *path, bool preview, bool download) {
    wchar_t *wide = win_wide(path);
    HANDLE h = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                    : INVALID_HANDLE_VALUE;
    DWORD error = GetLastError();
    free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        http_windows_error(c, r, error);
        return;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        error = GetLastError();
        CloseHandle(h);
        http_windows_error(c, r, error);
        return;
    }
    uint64_t length = (uint64_t)size.QuadPart;
    const char *type = http_content_type(path);
    if (preview && (!strcmp(type, "application/octet-stream") || !strncmp(type, "text/", 5))) {
        type = "text/plain; charset=utf-8";
        if (length > 256 * 1024)
            length = 256 * 1024;
    } else if (preview && length > HTTP_BODY_LIMIT) {
        CloseHandle(h);
        http_text(c, r, 413, "This file is too large to preview. Use Download.");
        return;
    }
    StringBuilder disposition = {0};
    if (download) {
        if (!sb_append(&disposition, "attachment; filename*=UTF-8''"))
            ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
            char escaped[4];
            snprintf(escaped, sizeof(escaped), "%%%02X", *p);
            if (!sb_append(&disposition, escaped))
                ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
        }
    }
    if (http_header(c, 200, type, length, disposition.data) && strcmp(r->method, "HEAD")) {
        char buffer[65536];
        while (length) {
            DWORD count, n = length > sizeof(buffer) ? sizeof(buffer) : (DWORD)length;
            if (!ReadFile(h, buffer, n, &count, NULL) || !count || !connection_write(c, buffer, count))
                break;
            length -= count;
        }
    }
    free(disposition.data);
    CloseHandle(h);
}
void http_static(Connection *c, const HttpRequest *r, const char *root) {
    if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
        http_text(c, r, 405, "Unsupported method.");
        return;
    }
    const char *relative = !strcmp(r->path, "/")           ? "index.html"
                           : !strncmp(r->path, "/web/", 5) ? r->path + 5
                                                           : r->path + 1;
    if (!*relative || *relative == '/' || strstr(relative, "..") || strchr(relative, '\\') ||
        strchr(relative, ':')) {
        http_text(c, r, 404, "File not found.");
        return;
    }
    char *path = win_join(root, relative);
    http_file(c, r, path, false, false);
    free(path);
}
