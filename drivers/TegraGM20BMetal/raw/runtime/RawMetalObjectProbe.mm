// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's metal.native.*: an object this device made itself answers its class and whether m2v's twin of it was ever
// made; an m2v object does not answer
@implementation RMObject (NativeProbe)

- (NSDictionary<NSString *, id> *)switchRawObject
{
    return @{@"class" : NSStringFromClass([self class]), @"twin" : @(_twin != nil)};
}

@end
#endif
