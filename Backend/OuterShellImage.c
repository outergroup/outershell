#include "OuterShellImage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void set_image_error(char *error, size_t error_size, const char *message) {
    if (!error || error_size == 0) return;
    snprintf(error, error_size, "%s", message ? message : "image processing failed");
}

#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>

static CFURLRef file_url(const char *path) {
    if (!path || !path[0]) return NULL;
    return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                   (const UInt8 *)path,
                                                   (CFIndex)strlen(path),
                                                   false);
}

bool outer_shell_png_dimensions(const char *path,
                                size_t *width,
                                size_t *height,
                                char *error,
                                size_t error_size) {
    if (width) *width = 0;
    if (height) *height = 0;
    CFURLRef url = file_url(path);
    if (!url) {
        set_image_error(error, error_size, "invalid PNG path");
        return false;
    }
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!source) {
        set_image_error(error, error_size, "could not open PNG");
        return false;
    }
    CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(source, 0, NULL);
    CFRelease(source);
    if (!properties) {
        set_image_error(error, error_size, "could not read PNG properties");
        return false;
    }
    CFNumberRef width_number = CFDictionaryGetValue(properties, kCGImagePropertyPixelWidth);
    CFNumberRef height_number = CFDictionaryGetValue(properties, kCGImagePropertyPixelHeight);
    long long decoded_width = 0;
    long long decoded_height = 0;
    bool ok = width_number && height_number &&
              CFNumberGetValue(width_number, kCFNumberLongLongType, &decoded_width) &&
              CFNumberGetValue(height_number, kCFNumberLongLongType, &decoded_height) &&
              decoded_width > 0 && decoded_height > 0;
    CFRelease(properties);
    if (!ok) {
        set_image_error(error, error_size, "PNG has invalid dimensions");
        return false;
    }
    if (width) *width = (size_t)decoded_width;
    if (height) *height = (size_t)decoded_height;
    return true;
}

bool outer_shell_resize_png(const char *source_path,
                            const char *destination_path,
                            size_t max_pixel_size,
                            char *error,
                            size_t error_size) {
    if (!source_path || !destination_path || max_pixel_size == 0) {
        set_image_error(error, error_size, "invalid PNG resize arguments");
        return false;
    }
    CFURLRef source_url = file_url(source_path);
    CFURLRef destination_url = file_url(destination_path);
    if (!source_url || !destination_url) {
        if (source_url) CFRelease(source_url);
        if (destination_url) CFRelease(destination_url);
        set_image_error(error, error_size, "invalid PNG resize path");
        return false;
    }
    CGImageSourceRef source = CGImageSourceCreateWithURL(source_url, NULL);
    CFRelease(source_url);
    if (!source) {
        CFRelease(destination_url);
        set_image_error(error, error_size, "could not decode PNG");
        return false;
    }

    int requested_size = max_pixel_size > INT32_MAX ? INT32_MAX : (int)max_pixel_size;
    CFNumberRef size_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &requested_size);
    const void *keys[] = {
        kCGImageSourceCreateThumbnailFromImageAlways,
        kCGImageSourceCreateThumbnailWithTransform,
        kCGImageSourceThumbnailMaxPixelSize,
        kCGImageSourceShouldCacheImmediately,
    };
    const void *values[] = {
        kCFBooleanTrue,
        kCFBooleanTrue,
        size_number,
        kCFBooleanTrue,
    };
    CFDictionaryRef options = size_number
        ? CFDictionaryCreate(kCFAllocatorDefault,
                             keys,
                             values,
                             sizeof(keys) / sizeof(keys[0]),
                             &kCFTypeDictionaryKeyCallBacks,
                             &kCFTypeDictionaryValueCallBacks)
        : NULL;
    CGImageRef thumbnail = options ? CGImageSourceCreateThumbnailAtIndex(source, 0, options) : NULL;
    if (options) CFRelease(options);
    if (size_number) CFRelease(size_number);
    CFRelease(source);
    if (!thumbnail) {
        CFRelease(destination_url);
        set_image_error(error, error_size, "could not resize PNG");
        return false;
    }

    CGImageDestinationRef destination = CGImageDestinationCreateWithURL(destination_url,
                                                                         CFSTR("public.png"),
                                                                         1,
                                                                         NULL);
    CFRelease(destination_url);
    bool ok = false;
    if (destination) {
        CGImageDestinationAddImage(destination, thumbnail, NULL);
        ok = CGImageDestinationFinalize(destination);
        CFRelease(destination);
    }
    CGImageRelease(thumbnail);
    if (!ok) {
        set_image_error(error, error_size, "could not write resized PNG");
        return false;
    }
    chmod(destination_path, 0644);
    return true;
}

#else

#include <png.h>
#include <setjmp.h>

typedef struct {
    char *error;
    size_t error_size;
    unsigned char *pixels;
    png_bytep *rows;
} OuterShellPNGContext;

static void outer_shell_libpng_error(png_structp png, png_const_charp message) {
    OuterShellPNGContext *context = png_get_error_ptr(png);
    if (context) set_image_error(context->error, context->error_size, message);
    longjmp(png_jmpbuf(png), 1);
}

static void outer_shell_libpng_warning(png_structp png, png_const_charp message) {
    (void)png;
    (void)message;
}

static bool read_png_signature(FILE *file, char *error, size_t error_size) {
    png_byte signature[8];
    if (fread(signature, 1, sizeof(signature), file) != sizeof(signature) ||
        png_sig_cmp(signature, 0, sizeof(signature)) != 0) {
        set_image_error(error, error_size, "file is not a PNG");
        return false;
    }
    return true;
}

bool outer_shell_png_dimensions(const char *path,
                                size_t *width,
                                size_t *height,
                                char *error,
                                size_t error_size) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (!path) {
        set_image_error(error, error_size, "invalid PNG path");
        return false;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        set_image_error(error, error_size, strerror(errno));
        return false;
    }
    if (!read_png_signature(file, error, error_size)) {
        fclose(file);
        return false;
    }

    OuterShellPNGContext *context = calloc(1, sizeof(*context));
    if (!context) {
        fclose(file);
        set_image_error(error, error_size, "not enough memory to read PNG");
        return false;
    }
    context->error = error;
    context->error_size = error_size;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             context,
                                             outer_shell_libpng_error,
                                             outer_shell_libpng_warning);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png) png_destroy_read_struct(&png, NULL, NULL);
        free(context);
        fclose(file);
        set_image_error(error, error_size, "could not initialize PNG reader");
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        free(context);
        fclose(file);
        return false;
    }

    png_init_io(png, file);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);
    png_uint_32 decoded_width = png_get_image_width(png, info);
    png_uint_32 decoded_height = png_get_image_height(png, info);
    bool ok = decoded_width > 0 && decoded_height > 0;
    if (ok) {
        if (width) *width = decoded_width;
        if (height) *height = decoded_height;
    } else {
        set_image_error(error, error_size, "PNG has invalid dimensions");
    }
    png_destroy_read_struct(&png, &info, NULL);
    free(context);
    fclose(file);
    if (!ok) return false;
    return true;
}

static bool read_png_rgba(const char *path,
                          png_uint_32 *width,
                          png_uint_32 *height,
                          unsigned char **pixels,
                          char *error,
                          size_t error_size) {
    *width = 0;
    *height = 0;
    *pixels = NULL;

    FILE *file = fopen(path, "rb");
    if (!file) {
        set_image_error(error, error_size, strerror(errno));
        return false;
    }
    if (!read_png_signature(file, error, error_size)) {
        fclose(file);
        return false;
    }

    OuterShellPNGContext *context = calloc(1, sizeof(*context));
    if (!context) {
        fclose(file);
        set_image_error(error, error_size, "not enough memory to decode PNG");
        return false;
    }
    context->error = error;
    context->error_size = error_size;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             context,
                                             outer_shell_libpng_error,
                                             outer_shell_libpng_warning);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png) png_destroy_read_struct(&png, NULL, NULL);
        free(context);
        fclose(file);
        set_image_error(error, error_size, "could not initialize PNG reader");
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        free(context->rows);
        free(context->pixels);
        free(context);
        fclose(file);
        return false;
    }

    png_init_io(png, file);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    png_uint_32 decoded_width;
    png_uint_32 decoded_height;
    int bit_depth;
    int color_type;
    if (!png_get_IHDR(png,
                      info,
                      &decoded_width,
                      &decoded_height,
                      &bit_depth,
                      &color_type,
                      NULL,
                      NULL,
                      NULL) ||
        decoded_width == 0 || decoded_height == 0 ||
        decoded_width > 16384 || decoded_height > 16384) {
        png_error(png, "PNG dimensions are unsupported");
    }

    bool has_transparency = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (has_transparency) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (!(color_type & PNG_COLOR_MASK_ALPHA) && !has_transparency) {
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    }
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    size_t row_bytes = png_get_rowbytes(png, info);
    if (png_get_bit_depth(png, info) != 8 ||
        png_get_channels(png, info) != 4 ||
        row_bytes != (size_t)decoded_width * 4 ||
        decoded_height > SIZE_MAX / row_bytes) {
        png_error(png, "PNG pixel format is unsupported");
    }

    context->pixels = malloc(row_bytes * decoded_height);
    context->rows = malloc(sizeof(*context->rows) * decoded_height);
    if (!context->pixels || !context->rows) {
        png_error(png, "not enough memory to decode PNG");
    }
    for (png_uint_32 row = 0; row < decoded_height; row++) {
        context->rows[row] = context->pixels + (size_t)row * row_bytes;
    }
    png_read_image(png, context->rows);
    png_read_end(png, info);

    *width = decoded_width;
    *height = decoded_height;
    *pixels = context->pixels;
    context->pixels = NULL;
    png_destroy_read_struct(&png, &info, NULL);
    free(context->rows);
    free(context);
    fclose(file);
    return true;
}

static bool write_png_rgba(const char *path,
                           png_uint_32 width,
                           png_uint_32 height,
                           unsigned char *pixels,
                           char *error,
                           size_t error_size) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        set_image_error(error, error_size, strerror(errno));
        return false;
    }

    OuterShellPNGContext *context = calloc(1, sizeof(*context));
    if (!context) {
        fclose(file);
        set_image_error(error, error_size, "not enough memory to write PNG");
        return false;
    }
    context->error = error;
    context->error_size = error_size;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              context,
                                              outer_shell_libpng_error,
                                              outer_shell_libpng_warning);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png) png_destroy_write_struct(&png, NULL);
        free(context);
        fclose(file);
        set_image_error(error, error_size, "could not initialize PNG writer");
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        free(context->rows);
        free(context);
        fclose(file);
        return false;
    }

    context->rows = malloc(sizeof(*context->rows) * height);
    if (!context->rows) png_error(png, "not enough memory to write PNG");
    for (png_uint_32 row = 0; row < height; row++) {
        context->rows[row] = pixels + (size_t)row * width * 4;
    }

    png_init_io(png, file);
    png_set_IHDR(png,
                 info,
                 width,
                 height,
                 8,
                 PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_write_image(png, context->rows);
    png_write_end(png, info);

    png_destroy_write_struct(&png, &info);
    free(context->rows);
    free(context);
    if (fclose(file) != 0) {
        set_image_error(error, error_size, strerror(errno));
        return false;
    }
    chmod(path, 0644);
    return true;
}

static unsigned char rounded_byte(double value) {
    if (value <= 0.0) return 0;
    if (value >= 255.0) return 255;
    return (unsigned char)(value + 0.5);
}

/* Area sampling avoids aliasing when large app icons are reduced to launcher size.
   Colors are accumulated premultiplied so transparent edges do not acquire halos. */
static void resize_rgba_area(const unsigned char *source,
                             size_t source_width,
                             size_t source_height,
                             unsigned char *destination,
                             size_t destination_width,
                             size_t destination_height) {
    for (size_t destination_y = 0; destination_y < destination_height; destination_y++) {
        double top = (double)destination_y * (double)source_height / (double)destination_height;
        double bottom = (double)(destination_y + 1) * (double)source_height / (double)destination_height;
        size_t first_y = (size_t)top;
        size_t last_y = (size_t)(bottom - 1e-12);
        for (size_t destination_x = 0; destination_x < destination_width; destination_x++) {
            double left = (double)destination_x * (double)source_width / (double)destination_width;
            double right = (double)(destination_x + 1) * (double)source_width / (double)destination_width;
            size_t first_x = (size_t)left;
            size_t last_x = (size_t)(right - 1e-12);
            double alpha_sum = 0.0;
            double red_sum = 0.0;
            double green_sum = 0.0;
            double blue_sum = 0.0;
            double area_sum = 0.0;
            for (size_t source_y = first_y; source_y <= last_y; source_y++) {
                double vertical_weight = (bottom < (double)(source_y + 1) ? bottom : (double)(source_y + 1)) -
                                         (top > (double)source_y ? top : (double)source_y);
                for (size_t source_x = first_x; source_x <= last_x; source_x++) {
                    double horizontal_weight = (right < (double)(source_x + 1) ? right : (double)(source_x + 1)) -
                                               (left > (double)source_x ? left : (double)source_x);
                    double area = horizontal_weight * vertical_weight;
                    const unsigned char *pixel = source + (source_y * source_width + source_x) * 4;
                    double alpha = (double)pixel[3] / 255.0;
                    red_sum += (double)pixel[0] * alpha * area;
                    green_sum += (double)pixel[1] * alpha * area;
                    blue_sum += (double)pixel[2] * alpha * area;
                    alpha_sum += alpha * area;
                    area_sum += area;
                }
            }
            unsigned char *output = destination + (destination_y * destination_width + destination_x) * 4;
            output[3] = rounded_byte(255.0 * alpha_sum / area_sum);
            if (alpha_sum > 0.0) {
                output[0] = rounded_byte(red_sum / alpha_sum);
                output[1] = rounded_byte(green_sum / alpha_sum);
                output[2] = rounded_byte(blue_sum / alpha_sum);
            } else {
                output[0] = output[1] = output[2] = 0;
            }
        }
    }
}

bool outer_shell_resize_png(const char *source_path,
                            const char *destination_path,
                            size_t max_pixel_size,
                            char *error,
                            size_t error_size) {
    if (!source_path || !destination_path || max_pixel_size == 0 || max_pixel_size > UINT32_MAX) {
        set_image_error(error, error_size, "invalid PNG resize arguments");
        return false;
    }
    png_uint_32 source_width;
    png_uint_32 source_height;
    unsigned char *source_pixels;
    if (!read_png_rgba(source_path,
                       &source_width,
                       &source_height,
                       &source_pixels,
                       error,
                       error_size)) return false;

    size_t destination_width;
    size_t destination_height;
    if (source_width >= source_height) {
        destination_width = max_pixel_size;
        destination_height = ((size_t)source_height * max_pixel_size + source_width / 2) / source_width;
    } else {
        destination_height = max_pixel_size;
        destination_width = ((size_t)source_width * max_pixel_size + source_height / 2) / source_height;
    }
    if (destination_width == 0) destination_width = 1;
    if (destination_height == 0) destination_height = 1;
    if (destination_width > SIZE_MAX / destination_height / 4) {
        free(source_pixels);
        set_image_error(error, error_size, "resized PNG dimensions overflow");
        return false;
    }
    size_t destination_size = destination_width * destination_height * 4;
    unsigned char *destination_pixels = malloc(destination_size);
    if (!destination_pixels) {
        free(source_pixels);
        set_image_error(error, error_size, "not enough memory to resize PNG");
        return false;
    }
    resize_rgba_area(source_pixels,
                     source_width,
                     source_height,
                     destination_pixels,
                     destination_width,
                     destination_height);
    free(source_pixels);
    bool ok = write_png_rgba(destination_path,
                             (png_uint_32)destination_width,
                             (png_uint_32)destination_height,
                             destination_pixels,
                             error,
                             error_size);
    free(destination_pixels);
    return ok;
}

#endif
