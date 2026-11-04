// Runtime configuration. See AKDMConfig.h.

#include "AKDMConfig.h"
#include "AKDMCompat.h"

#import <Foundation/Foundation.h>

namespace akdm {

Config LoadConfig() {
    Config config;
    @autoreleasepool {
        NSString *directory = [NSHomeDirectory()
            stringByAppendingPathComponent:@"Documents/AKDM"];
        NSString *path = [directory stringByAppendingPathComponent:@"config.plist"];
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (data == nil) {
            AKDM_LOG("config: none at %s (using defaults)",
                     path.UTF8String ?: "(null)");
            return config;
        }
        NSError *error = nil;
        id parsed = [NSPropertyListSerialization propertyListWithData:data
                                                             options:0
                                                              format:nullptr
                                                               error:&error];
        if (![parsed isKindOfClass:[NSDictionary class]]) {
            AKDM_LOG("config: unreadable (%s)", error.localizedDescription.UTF8String
                                                    ?: "not a dictionary");
            return config;
        }
        NSDictionary *dictionary = (NSDictionary *)parsed;
        auto readBool = [&](NSString *key, bool fallback) {
            id value = dictionary[key];
            return [value isKindOfClass:[NSNumber class]] ? [value boolValue]
                                                          : fallback;
        };
        config.enablePCMode = readBool(@"EnablePCMode", config.enablePCMode);
        config.enableInputHooks =
            readBool(@"EnableInputHooks", config.enableInputHooks);
        config.enableTouchEmulation =
            readBool(@"EnableTouchEmulation", config.enableTouchEmulation);

        id sensitivity = dictionary[@"MouseSensitivity"];
        if ([sensitivity isKindOfClass:[NSNumber class]]) {
            const double value = [sensitivity doubleValue];
            if (value > 0.05 && value < 20.0) config.mouseSensitivity = value;
        }
        AKDM_LOG("config: pc=%d hooks=%d touch=%d sensitivity=%.2f",
                 config.enablePCMode ? 1 : 0, config.enableInputHooks ? 1 : 0,
                 config.enableTouchEmulation ? 1 : 0, config.mouseSensitivity);
    }
    return config;
}

} // namespace akdm
