#import <Foundation/Foundation.h>
#import "OuterframeAppConnection.h"

NS_ASSUME_NONNULL_BEGIN

@protocol OuterframeContentLibrary <NSObject>

@optional
+ (int32_t)startWithSocketFD:(int32_t)socketFD appConnection:(id<OuterframeAppConnection>)appConnection;

@end

NS_ASSUME_NONNULL_END
