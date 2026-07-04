#import "HelloFullstackContent.h"
#import "Vendor/OuterframeC/OuterframeHost.h"

#import <AppKit/AppKit.h>
#import <CFNetwork/CFNetwork.h>
#import <stdlib.h>

typedef struct {
    OFHost *host;
    __strong id<OuterframeAppConnection> app_connection;
    __strong NSAppearance *appearance;
    __strong CALayer *root_layer;
    __strong CATextLayer *title_layer;
    __strong CATextLayer *subtitle_layer;
    CGSize current_size;
    bool destroy_scheduled;
} HelloFullstackApp;

static NSString *HelloFullstackStringFromView(OFStringView view) {
    if (!view.bytes || view.length == 0) {
        return @"";
    }
    return [[NSString alloc] initWithBytes:view.bytes
                                    length:view.length
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

static NSURL *HelloFullstackBackendURL(OFHost *host) {
    const char *url_cstring = OFHostURL(host);
    if (!url_cstring || url_cstring[0] == '\0') {
        return nil;
    }

    NSString *url_string = [NSString stringWithUTF8String:url_cstring];
    NSURLComponents *components = [NSURLComponents componentsWithString:url_string];
    if (!components) {
        return nil;
    }

    components.path = @"/api/hello";
    components.query = nil;
    components.fragment = nil;
    return components.URL;
}

static void HelloFullstackApplyProxy(NSURLSessionConfiguration *configuration, const OFInitializeContentProxy *proxy) {
    if (!proxy || !proxy->present || proxy->port == 0 || proxy->host.length == 0) {
        return;
    }

    NSString *host = HelloFullstackStringFromView(proxy->host);
    if (host.length == 0) {
        return;
    }

    NSMutableDictionary *proxy_dictionary = [NSMutableDictionary dictionary];
    proxy_dictionary[(NSString *)kCFStreamPropertySOCKSProxyHost] = host;
    proxy_dictionary[(NSString *)kCFStreamPropertySOCKSProxyPort] = @(proxy->port);
    if (proxy->has_username) {
        proxy_dictionary[(NSString *)kCFStreamPropertySOCKSUser] = HelloFullstackStringFromView(proxy->username);
    }
    if (proxy->has_password) {
        proxy_dictionary[(NSString *)kCFStreamPropertySOCKSPassword] = HelloFullstackStringFromView(proxy->password);
    }
    configuration.connectionProxyDictionary = proxy_dictionary;
}

static NSString *HelloFullstackBackendSubtitleFromData(NSData *data) {
    if (!data) {
        return nil;
    }

    NSError *error = nil;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if (![json isKindOfClass:NSDictionary.class]) {
        return nil;
    }

    NSDictionary *object = (NSDictionary *)json;
    NSString *message = [object[@"message"] isKindOfClass:NSString.class] ? object[@"message"] : nil;
    NSString *hostname = [object[@"hostname"] isKindOfClass:NSString.class] ? object[@"hostname"] : nil;
    NSString *os = [object[@"os"] isKindOfClass:NSString.class] ? object[@"os"] : nil;
    if (!message || !hostname || !os) {
        return nil;
    }
    return [NSString stringWithFormat:@"%@ — %@ on %@", message, os, hostname];
}

static void HelloFullstackFetchBackendGreeting(HelloFullstackApp *app, OFHost *host, const OFInitializeContent *initialize) {
    NSURL *api_url = HelloFullstackBackendURL(host);
    if (!api_url) {
        app->subtitle_layer.string = @"No origin URL available.";
        return;
    }

    CATextLayer *subtitle_layer = app->subtitle_layer;
    subtitle_layer.string = @"Asking the backend for a greeting...";

    NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    HelloFullstackApplyProxy(configuration, &initialize->proxy);
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration];
    NSURLSessionDataTask *task = [session dataTaskWithURL:api_url
                                        completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        (void)response;
        NSString *subtitle = nil;
        if (error) {
            subtitle = [NSString stringWithFormat:@"Backend request failed: %@", error.localizedDescription];
        } else {
            subtitle = HelloFullstackBackendSubtitleFromData(data) ?: @"Backend response was invalid.";
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            subtitle_layer.string = subtitle;
        });
        [session finishTasksAndInvalidate];
    }];
    [task resume];
}

static void HelloFullstackSetAppearance(HelloFullstackApp *app, NSAppearance *appearance) {
    app->appearance = appearance ?: NSAppearance.currentDrawingAppearance;
}

static void HelloFullstackConfigureLayersIfNeeded(HelloFullstackApp *app) {
    CALayer *root_layer = app->root_layer;
    CATextLayer *title_layer = app->title_layer;
    CATextLayer *subtitle_layer = app->subtitle_layer;

    if (title_layer.superlayer) {
        return;
    }

    title_layer.string = @"Hello World";
    title_layer.font = (__bridge CFTypeRef)[NSFont systemFontOfSize:34 weight:NSFontWeightSemibold];
    title_layer.fontSize = 34;
    title_layer.alignmentMode = kCAAlignmentCenter;
    title_layer.contentsScale = 2.0;
    title_layer.wrapped = YES;

    subtitle_layer.string = @"This is an outerframe app.";
    subtitle_layer.font = (__bridge CFTypeRef)[NSFont systemFontOfSize:15 weight:NSFontWeightRegular];
    subtitle_layer.fontSize = 15;
    subtitle_layer.alignmentMode = kCAAlignmentCenter;
    subtitle_layer.contentsScale = 2.0;
    subtitle_layer.wrapped = YES;

    [root_layer addSublayer:title_layer];
    [root_layer addSublayer:subtitle_layer];
}

static void HelloFullstackUpdateLayout(HelloFullstackApp *app) {
    CGFloat width = MAX(app->current_size.width, 1);
    CGFloat height = MAX(app->current_size.height, 1);

    [CATransaction begin];
    [CATransaction setDisableActions:YES];

    app->root_layer.frame = CGRectMake(0, 0, width, height);
    CGFloat horizontal_padding = MIN(MAX(width * 0.1, 24), 80);
    app->title_layer.frame = CGRectMake(horizontal_padding, height * 0.5, width - (horizontal_padding * 2), 44);
    app->subtitle_layer.frame = CGRectMake(horizontal_padding, MAX(CGRectGetMinY(app->title_layer.frame) - 48, 24), width - (horizontal_padding * 2), 40);

    [CATransaction commit];
}

static void HelloFullstackUpdateColors(HelloFullstackApp *app) {
    NSAppearance *appearance = app->appearance ?: NSAppearance.currentDrawingAppearance;

    [appearance performAsCurrentDrawingAppearance:^{
        app->root_layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
        app->title_layer.foregroundColor = NSColor.labelColor.CGColor;
        app->subtitle_layer.foregroundColor = NSColor.secondaryLabelColor.CGColor;
    }];
}

static bool HelloFullstackWriteAccessibilitySnapshot(HelloFullstackApp *app, OFBuffer *out_snapshot_data) {
    NSString *title = [app->title_layer.string isKindOfClass:NSString.class] ? (NSString *)app->title_layer.string : @"Hello World";
    NSString *subtitle = [app->subtitle_layer.string isKindOfClass:NSString.class] ? (NSString *)app->subtitle_layer.string : @"";

    OFAccessibilityNode nodes[3] = {
        {
            .identifier = 1,
            .role = OFAccessibilityRoleStaticText,
            .frame = app->title_layer.frame,
            .label = title.UTF8String,
            .enabled = true,
        },
        {
            .identifier = 2,
            .role = OFAccessibilityRoleStaticText,
            .frame = app->subtitle_layer.frame,
            .label = subtitle.UTF8String,
            .enabled = true,
        },
        {
            .identifier = 0,
            .role = OFAccessibilityRoleContainer,
            .frame = app->root_layer.frame,
            .label = "Hello world outerframe app",
            .enabled = true,
        },
    };
    nodes[2].children = nodes;
    nodes[2].child_count = 2;

    OFAccessibilitySnapshot snapshot = {
        .root_nodes = &nodes[2],
        .root_count = 1,
    };
    return OFAccessibilitySnapshotEncode(&snapshot, out_snapshot_data);
}

static void HelloFullstackAppDestroy(HelloFullstackApp *app) {
    if (!app) return;
    if (app->host) {
        OFHostDestroy(app->host);
        app->host = NULL;
    }
    app->app_connection = nil;
    app->appearance = nil;
    app->root_layer = nil;
    app->title_layer = nil;
    app->subtitle_layer = nil;
    free(app);
}

static void HelloFullstackScheduleDestroy(HelloFullstackApp *app) {
    if (!app || app->destroy_scheduled) {
        return;
    }
    app->destroy_scheduled = true;
    dispatch_async(dispatch_get_main_queue(), ^{
        HelloFullstackAppDestroy(app);
    });
}

static void HelloFullstackHandleMessage(OFHost *host, const OFBrowserMessage *message, void *context) {
    HelloFullstackApp *app = context;
    switch (message->kind) {
        case OFBrowserMessageInitializeContent: {
            const OFInitializeContent *initialize = &message->as.initialize;
            OFHostConfigureFromInitialize(host, initialize);

            NSAppearance *appearance;
            if (initialize->has_appearance_archive) {
                NSData *data = [NSData dataWithBytesNoCopy:(void *)initialize->appearance_archive.bytes
                                                     length:initialize->appearance_archive.length
                                               freeWhenDone:NO];
                appearance = [NSKeyedUnarchiver unarchivedObjectOfClass:NSAppearance.class fromData:data error:nil];
            } else {
                appearance = NSAppearance.currentDrawingAppearance;
            }

            HelloFullstackSetAppearance(app, appearance);
            app->current_size = initialize->has_content_size ? initialize->content_size : CGSizeMake(800, 600);

            HelloFullstackConfigureLayersIfNeeded(app);
            HelloFullstackUpdateLayout(app);
            HelloFullstackUpdateColors(app);

            if ([app->app_connection respondsToSelector:@selector(registerLayer:)]) {
                [app->app_connection registerLayer:app->root_layer];
            }

            OFHostSetTitle(host, "Hello World");
            OFHostSetIconBundleResource(host, "Contents/Resources/app-icon.png");
            OFHostUpdateStartPageMetadata(host, "Hello World", NULL, 0, 0, 0);
            OFHostUpdatePageMetadata(host, "Hello World", NULL, 0, 0, 0);
            HelloFullstackFetchBackendGreeting(app, host, initialize);
            break;
        }

        case OFBrowserMessageResizeContent:
            app->current_size = message->as.resize;
            HelloFullstackUpdateLayout(app);
            break;

        case OFBrowserMessageSystemAppearanceUpdate:  {
            NSData *data = [NSData dataWithBytesNoCopy:(void *)message->as.appearance.appearance_archive.bytes
                                                 length:message->as.appearance.appearance_archive.length
                                           freeWhenDone:NO];
            NSAppearance *appearance = [NSKeyedUnarchiver unarchivedObjectOfClass:NSAppearance.class fromData:data error:nil];

            HelloFullstackSetAppearance(app, appearance ?: NSAppearance.currentDrawingAppearance);
            HelloFullstackUpdateColors(app);
            break;
        }

        case OFBrowserMessageAccessibilitySnapshotRequest: {
            OFBuffer snapshot_data = {0};
            if (!HelloFullstackWriteAccessibilitySnapshot(app, &snapshot_data)) {
                OFAccessibilityNotImplementedSnapshot("Accessibility not implemented", &snapshot_data);
            }
            OFHostSendAccessibilitySnapshotResponse(host, message->as.request.request_id, snapshot_data.bytes, snapshot_data.length);
            OFBufferFree(&snapshot_data);
            break;
        }

        case OFBrowserMessageShutdown:
            HelloFullstackScheduleDestroy(app);
            break;

        default:
            break;
    }
}

static void HelloFullstackHandleDisconnect(OFHost *host, void *context) {
    (void)host;
    HelloFullstackScheduleDestroy(context);
}

static HelloFullstackApp *HelloFullstackAppCreate(int32_t socket_fd, id<OuterframeAppConnection> app_connection) {
    HelloFullstackApp *app = calloc(1, sizeof(*app));
    if (!app) {
        return NULL;
    }

    app->app_connection = app_connection;
    app->root_layer = [CALayer layer];
    app->title_layer = [CATextLayer layer];
    app->subtitle_layer = [CATextLayer layer];
    app->current_size = CGSizeMake(800, 600);

    OFHostCallbacks callbacks = {
        .message = HelloFullstackHandleMessage,
        .disconnected = HelloFullstackHandleDisconnect,
    };
    app->host = OFHostCreate(socket_fd, callbacks, app);
    if (!app->host) {
        HelloFullstackAppDestroy(app);
        return NULL;
    }

    return app;
}

@implementation HelloFullstackContent

+ (int32_t)startWithSocketFD:(int32_t)socketFD appConnection:(id<OuterframeAppConnection>)appConnection {
    return HelloFullstackAppCreate(socketFD, appConnection) ? 0 : 1;
}

@end
