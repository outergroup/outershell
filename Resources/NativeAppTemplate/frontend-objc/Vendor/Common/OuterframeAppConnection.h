#import <QuartzCore/QuartzCore.h>

NS_ASSUME_NONNULL_BEGIN

@protocol OuterframeAppConnection <NSObject>

@optional
- (void)registerLayer:(CALayer *)layer;

@end

NS_ASSUME_NONNULL_END
