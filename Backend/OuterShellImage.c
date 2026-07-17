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

bool outer_shell_png_dimensions(const char *path,
                                size_t *width,
                                size_t *height,
                                char *error,
                                size_t error_size) {
    if (width) *width = 0;
    if (height) *height = 0;
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!path || !png_image_begin_read_from_file(&image, path)) {
        set_image_error(error, error_size, image.message[0] ? image.message : "could not open PNG");
        return false;
    }
    if (width) *width = image.width;
    if (height) *height = image.height;
    png_image_free(&image);
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
    png_image source_image;
    memset(&source_image, 0, sizeof(source_image));
    source_image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&source_image, source_path)) {
        set_image_error(error, error_size, source_image.message[0] ? source_image.message : "could not open PNG");
        return false;
    }
    if (source_image.width == 0 || source_image.height == 0 ||
        source_image.width > 16384 || source_image.height > 16384) {
        png_image_free(&source_image);
        set_image_error(error, error_size, "PNG dimensions are unsupported");
        return false;
    }
    source_image.format = PNG_FORMAT_RGBA;
    size_t source_size = PNG_IMAGE_SIZE(source_image);
    unsigned char *source_pixels = malloc(source_size);
    if (!source_pixels) {
        png_image_free(&source_image);
        set_image_error(error, error_size, "not enough memory to decode PNG");
        return false;
    }
    if (!png_image_finish_read(&source_image, NULL, source_pixels, 0, NULL)) {
        set_image_error(error, error_size, source_image.message[0] ? source_image.message : "could not decode PNG");
        free(source_pixels);
        png_image_free(&source_image);
        return false;
    }

    size_t destination_width;
    size_t destination_height;
    if (source_image.width >= source_image.height) {
        destination_width = max_pixel_size;
        destination_height = ((size_t)source_image.height * max_pixel_size + source_image.width / 2) / source_image.width;
    } else {
        destination_height = max_pixel_size;
        destination_width = ((size_t)source_image.width * max_pixel_size + source_image.height / 2) / source_image.height;
    }
    if (destination_width == 0) destination_width = 1;
    if (destination_height == 0) destination_height = 1;
    if (destination_width > SIZE_MAX / destination_height / 4) {
        free(source_pixels);
        png_image_free(&source_image);
        set_image_error(error, error_size, "resized PNG dimensions overflow");
        return false;
    }
    size_t destination_size = destination_width * destination_height * 4;
    unsigned char *destination_pixels = malloc(destination_size);
    if (!destination_pixels) {
        free(source_pixels);
        png_image_free(&source_image);
        set_image_error(error, error_size, "not enough memory to resize PNG");
        return false;
    }
    resize_rgba_area(source_pixels,
                     source_image.width,
                     source_image.height,
                     destination_pixels,
                     destination_width,
                     destination_height);
    free(source_pixels);
    png_image_free(&source_image);

    png_image destination_image;
    memset(&destination_image, 0, sizeof(destination_image));
    destination_image.version = PNG_IMAGE_VERSION;
    destination_image.width = (png_uint_32)destination_width;
    destination_image.height = (png_uint_32)destination_height;
    destination_image.format = PNG_FORMAT_RGBA;
    bool ok = png_image_write_to_file(&destination_image,
                                      destination_path,
                                      0,
                                      destination_pixels,
                                      0,
                                      NULL) != 0;
    if (!ok) {
        set_image_error(error,
                        error_size,
                        destination_image.message[0] ? destination_image.message : "could not write resized PNG");
    } else {
        chmod(destination_path, 0644);
    }
    free(destination_pixels);
    png_image_free(&destination_image);
    return ok;
}

#endif
