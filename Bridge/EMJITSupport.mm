#import "EMJITSupport.h"

#import "EMEmulatorCore.h"
#import "../Core/src/cpu/jit/jit_memory.h"

#import <TargetConditionals.h>
#import <os/proc.h>
#import <string>
#import <unistd.h>

// Security.framework SPI: reads an entitlement out of our own code signature.
// Fine for sideloaded builds; it would not pass App Store review.
extern "C" {
typedef struct __SecTask *SecTaskRef;
SecTaskRef _Nullable SecTaskCreateFromSelf(CFAllocatorRef _Nullable allocator);
CFTypeRef _Nullable SecTaskCopyValueForEntitlement(SecTaskRef task, CFStringRef entitlement,
                                                   CFErrorRef _Nullable *_Nullable error);
}

// Returns the first directory entry under `directory` whose name is exactly
// `length` characters long (a UUID is 36, a boot manifest hash is 96).
static NSString *_Nullable EMFirstEntryWithNameLength(NSString *directory, NSUInteger length) {
    NSArray<NSString *> *entries = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:nil];
    for (NSString *name in entries) {
        if (name.length == length) return [directory stringByAppendingPathComponent:name];
    }
    return nil;
}

static BOOL EMFail(NSError **error, NSString *message) {
    if (error) {
        *error = [NSError errorWithDomain:EMEmulatorErrorDomain
                                     code:10
                                 userInfo:@{NSLocalizedDescriptionKey : message}];
    }
    return NO;
}

// Process-wide: lives until exit, shared by consecutive game sessions.
static ember::jit::CodeRegion gCodeCache;

@implementation EMJITSupport

+ (BOOL)hasGetTaskAllow {
#if TARGET_OS_SIMULATOR
    return YES;
#else
    static BOOL result;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        SecTaskRef task = SecTaskCreateFromSelf(NULL);
        if (!task) return;
        CFTypeRef value = SecTaskCopyValueForEntitlement(task, CFSTR("get-task-allow"), NULL);
        result = value == kCFBooleanTrue;
        if (value) CFRelease(value);
        CFRelease((CFTypeRef)task);
    });
    return result;
#endif
}

+ (BOOL)isDebugged {
    return ember::jit::processIsDebugged();
}

+ (EMTXMStatus)txmStatus {
#if TARGET_OS_SIMULATOR
    return EMTXMStatusAbsent;
#else
    static EMTXMStatus status;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (@available(iOS 26.0, *)) {
            // Heuristic used by current sideloaded JIT apps: devices that boot a
            // Trusted Execution Monitor ship its firmware image in the preboot
            // volume. Not an API — re-verify on new iOS releases. "Unknown" is
            // reported (not "absent") when the volume cannot be read, and the UI
            // offers a manual override for that case.
            NSString *volume = EMFirstEntryWithNameLength(@"/System/Volumes/Preboot", 36);
            NSString *boot   = volume ? EMFirstEntryWithNameLength([volume stringByAppendingPathComponent:@"boot"], 96) : nil;
            if (!boot) {
                status = EMTXMStatusUnknown;
                return;
            }
            NSString *image = [boot stringByAppendingPathComponent:
                @"usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4"];
            status = access(image.fileSystemRepresentation, F_OK) == 0 ? EMTXMStatusPresent : EMTXMStatusAbsent;
        } else {
            status = EMTXMStatusAbsent; // before iOS 26 a debugger attach is sufficient everywhere
        }
    });
    return status;
#endif
}

+ (BOOL)isInsideLiveContainer {
    return getenv("LC_HOME_PATH") != NULL;
}

+ (uint64_t)availableMemoryBytes {
    return (uint64_t)os_proc_available_memory();
}

+ (uint64_t)physicalMemoryBytes {
    return NSProcessInfo.processInfo.physicalMemory;
}

+ (BOOL)prepareCodeCacheOfSize:(NSUInteger)bytes useTXMProtocol:(BOOL)useTXMProtocol error:(NSError **)error {
    using namespace ember::jit;
    @synchronized(self) {
        if (gCodeCache) return YES;
        if (!processIsDebugged()) {
            return EMFail(error, @"No debugger has attached yet, so iOS will not allow executable memory.");
        }

        const Policy policy = selectPolicy(useTXMProtocol);
        CodeRegion region;
        std::string reason;
        const bool allocated = allocateRegion(policy, bytes, region, reason);
        // Every region must exist before this call: after detaching, the script
        // can no longer prepare new ones. It also runs on failure, so a waiting
        // script is always released.
        finishSetup(policy);
        if (!allocated) {
            return EMFail(error, [NSString stringWithFormat:@"Could not create the code cache: %s", reason.c_str()]);
        }
        if (!smokeTest(region)) {
            freeRegion(region);
            return EMFail(error, @"The code cache was created but did not execute correctly.");
        }
        gCodeCache = region;
        return YES;
    }
}

+ (BOOL)isCodeCacheReady {
    @synchronized(self) {
        return static_cast<bool>(gCodeCache);
    }
}

+ (BOOL)attachCodeCacheToCore:(EMEmulatorCore *)core {
    @synchronized(self) {
        if (!gCodeCache) return NO;
        em_core_set_jit_region(core.handle, gCodeCache.rx, gCodeCache.rw, gCodeCache.size);
        return YES;
    }
}

@end
