#define COBJMACROS
#include "Icons.h"
#include <bcrypt.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct IconCapability {
    struct IconCapability *next;
    char key[65], token[33];
} IconCapability;
static SRWLOCK icon_lock = SRWLOCK_INIT;
static char *icon_directory;
static IconCapability *capabilities;
static size_t capability_count;
static void hex_string(const unsigned char *bytes, size_t length, char *out) {
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) { out[i * 2] = hex[bytes[i] >> 4]; out[i * 2 + 1] = hex[bytes[i] & 15]; }
    out[length * 2] = 0;
}
bool icons_init(const char *state) {
    char *directory = win_join(state, "discovered-icons");
    if (!directory || !win_secure_directory(directory)) { free(directory); return false; }
    AcquireSRWLockExclusive(&icon_lock);
    free(icon_directory); icon_directory = directory;
    while (capabilities) { IconCapability *next = capabilities->next; free(capabilities); capabilities = next; }
    capability_count = 0;
    ReleaseSRWLockExclusive(&icon_lock);
    return true;
}
static char *icon_path(const char *key) {
    char name[70]; snprintf(name, sizeof(name), "%s.png", key);
    return icon_directory ? win_join(icon_directory, name) : NULL;
}
void icons_describe(const char *const *identity, size_t count, StringBuilder *image, char token[33]) {
    token[0] = 0;
    if (image->length) return;
    BCRYPT_ALG_HANDLE algorithm = NULL; BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32]; char key[65];
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0;
    for (size_t i = 0; ok && i < count; i++) {
        const char *field = identity[i] ? identity[i] : "";
        ok = BCryptHashData(hash, (PUCHAR)field, (ULONG)strlen(field) + 1, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return;
    hex_string(digest, sizeof(digest), key);
    AcquireSRWLockExclusive(&icon_lock);
    char *path = icon_path(key);
    if (!path) goto done;
    if (win_read_file(path, 48 * 1024, image) && image->length) goto done;
    free(image->data); *image = (StringBuilder){0};
    for (IconCapability *c = capabilities; c; c = c->next) {
        if (!strcmp(c->key, key)) { memcpy(token, c->token, 33); goto done; }
    }
    unsigned char random[16];
    if (capability_count >= 4096 || BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) goto done;
    IconCapability *c = calloc(1, sizeof(*c));
    if (!c) goto done;
    memcpy(c->key, key, sizeof(key)); hex_string(random, sizeof(random), c->token);
    c->next = capabilities; capabilities = c; capability_count++;
    memcpy(token, c->token, 33);
done:
    free(path);
    ReleaseSRWLockExclusive(&icon_lock);
}
static bool valid_png(const void *bytes, size_t length) {
    if (!bytes || length < 8 || length > 48 * 1024 || memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) return false;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
    IWICImagingFactory *factory = NULL; IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL; IWICBitmapFrameDecode *frame = NULL;
    UINT width = 0, height = 0; GUID format;
    bool ok = SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory)) &&
        SUCCEEDED(IWICImagingFactory_CreateStream(factory, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)bytes, (DWORD)length)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(IWICBitmapDecoder_GetContainerFormat(decoder, &format)) && IsEqualGUID(&format, &GUID_ContainerFormatPng) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &width, &height)) && width && height && width <= 4096 && height <= 4096;
    IWICFormatConverter *converter = NULL;
    BYTE *pixels = ok ? malloc((size_t)width * height * 4) : NULL;
    if (ok) ok = pixels && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory, &converter)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame,
            &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_CopyPixels(converter, NULL, width * 4, width * height * 4, pixels));
    free(pixels);
    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return ok;
}
int icons_save(const char *token, const void *bytes, size_t length) {
    if (!token || strlen(token) != 32) return 403;
    AcquireSRWLockExclusive(&icon_lock);
    IconCapability *capability;
    for (capability = capabilities; capability; capability = capability->next)
        if (!strcmp(capability->token, token)) break;
    int status = 403;
    if (!capability) goto done;
    if (!valid_png(bytes, length)) { status = 400; goto done; }
    char *path = icon_path(capability->key);
    StringBuilder existing = {0};
    if (path && win_read_file(path, 48 * 1024, &existing) && existing.length) status = 200;
    else status = path && win_atomic_write(path, bytes, length) ? 200 : 500;
    free(existing.data); free(path);
done:
    ReleaseSRWLockExclusive(&icon_lock);
    return status;
}
