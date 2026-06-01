#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>

#include "OuterShellDownloader.h"

#if defined(__APPLE__)

static void set_download_error(char *error, size_t error_size, NSString *message) {
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", message ? message.UTF8String : "download failed");
    }
}

static NSData *fetch_url_data(NSString *urlString, NSString **errorMessage) {
    NSURL *url = [NSURL URLWithString:urlString];
    if (!url) {
        if (errorMessage) *errorMessage = @"invalid URL";
        return nil;
    }

    NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.timeoutIntervalForRequest = 30.0;
    configuration.timeoutIntervalForResource = 120.0;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration];

    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    __block NSData *responseData = nil;
    __block NSURLResponse *response = nil;
    __block NSError *requestError = nil;

    NSURLSessionDataTask *task = [session dataTaskWithURL:url
                                        completionHandler:^(NSData *data, NSURLResponse *taskResponse, NSError *error) {
        responseData = data;
        response = taskResponse;
        requestError = error;
        dispatch_semaphore_signal(semaphore);
    }];
    [task resume];
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
    [session finishTasksAndInvalidate];

    if (requestError) {
        if (errorMessage) *errorMessage = requestError.localizedDescription;
        return nil;
    }
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
        NSInteger statusCode = [(NSHTTPURLResponse *)response statusCode];
        if (statusCode < 200 || statusCode >= 300) {
            if (errorMessage) {
                *errorMessage = [NSString stringWithFormat:@"HTTP status %ld", (long)statusCode];
            }
            return nil;
        }
    }
    return responseData;
}

bool outer_shell_download_url_to_file(const char *url,
                                      const char *path,
                                      char *error,
                                      size_t error_size) {
    @autoreleasepool {
        if (!url || !url[0] || !path || !path[0]) {
            set_download_error(error, error_size, @"missing download URL or path");
            return false;
        }
        NSString *errorMessage = nil;
        NSData *data = fetch_url_data([NSString stringWithUTF8String:url], &errorMessage);
        if (!data) {
            set_download_error(error, error_size, errorMessage);
            return false;
        }
        NSString *destination = [NSString stringWithUTF8String:path];
        NSError *writeError = nil;
        if (![data writeToFile:destination options:NSDataWritingAtomic error:&writeError]) {
            set_download_error(error, error_size, writeError.localizedDescription);
            return false;
        }
        return true;
    }
}

bool outer_shell_fetch_url_text(const char *url,
                                char *out,
                                size_t out_size,
                                char *error,
                                size_t error_size) {
    @autoreleasepool {
        if (out && out_size > 0) out[0] = '\0';
        if (!url || !url[0] || !out || out_size == 0) {
            set_download_error(error, error_size, @"missing URL or output buffer");
            return false;
        }
        NSString *errorMessage = nil;
        NSData *data = fetch_url_data([NSString stringWithUTF8String:url], &errorMessage);
        if (!data) {
            set_download_error(error, error_size, errorMessage);
            return false;
        }
        NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        if (!text) {
            set_download_error(error, error_size, @"response was not UTF-8");
            return false;
        }
        snprintf(out, out_size, "%s", text.UTF8String);
        return true;
    }
}

#endif
