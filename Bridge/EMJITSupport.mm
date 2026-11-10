#import "EMJITSupport.h"

#import "EMEmulatorCore.h"
#import "../Core/src/cpu/jit/jit_memory.h"

#import <TargetConditionals.h>
#import <fcntl.h>
#import <mach/machine.h>
#import <os/proc.h>
#import <sys/sysctl.h>
#import <sys/time.h>
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

// TXM by chip family, for when the firmware check cannot answer.
static EMTXMStatus EMTXMStatusFromChip(void) {
    uint32_t family = 0;
    size_t size = sizeof(family);
    if (sysctlbyname("hw.cpufamily", &family, &size, NULL, 0) != 0 || family == 0) return EMTXMStatusUnknown;
    switch (family) {
        case CPUFAMILY_ARM_VORTEX_TEMPEST:     // A12, A12X, A12Z
        case CPUFAMILY_ARM_LIGHTNING_THUNDER:  // A13
        case CPUFAMILY_ARM_FIRESTORM_ICESTORM: // A14, M1
            return EMTXMStatusAbsent;
        default:
            return EMTXMStatusPresent;         // A15 / M2 and later
    }
}

// MARK: - Trace file

static const char *EMTracePath(void) {
    static char path[1024];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURL *dir = [[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject
            URLByAppendingPathComponent:@"Ember" isDirectory:YES];
        [NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
        strlcpy(path, [dir URLByAppendingPathComponent:@"jit-trace.txt"].fileSystemRepresentation, sizeof(path));
    });
    return path;
}

// One open/write/close per line. Data handed to write() survives the process
// being killed, which is the only failure this has to outlive.
static void EMTraceLine(const char *message) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct tm parts;
    localtime_r(&now.tv_sec, &parts);
    char line[320];
    const int length = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d  %s\n", parts.tm_hour, parts.tm_min, parts.tm_sec,
                                (int)(now.tv_usec / 1000), message);
    const int fd = open(EMTracePath(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    if (length > 0) (void)write(fd, line, (size_t)MIN(length, (int)sizeof(line) - 1));
    close(fd);
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
            // 1. Firmware check: devices that boot a Trusted Execution Monitor
            //    ship its image in the preboot volume. Only a positive result is
            //    trusted — on iPadOS 27.0.1 (iPad14,8) the volume could not be
            //    read at all, which says nothing either way.
            NSString *volume = EMFirstEntryWithNameLength(@"/System/Volumes/Preboot", 36);
            NSString *boot   = volume ? EMFirstEntryWithNameLength([volume stringByAppendingPathComponent:@"boot"], 96) : nil;
            NSString *image  = [boot stringByAppendingPathComponent:
                @"usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4"];
            if (image && access(image.fileSystemRepresentation, F_OK) == 0) {
                status = EMTXMStatusPresent;
                return;
            }
            // 2. Chip generation: TXM arrived with the A15 / M2 generation. The
            //    older families that can still run iOS 26+ are listed; anything
            //    newer, including chips released after this was written, has it.
            status = EMTXMStatusFromChip();
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

+ (BOOL)prepareCodeCacheOfSize:(NSUInteger)bytes
                useTXMProtocol:(BOOL)useTXMProtocol
             debuggerAllocates:(BOOL)debuggerAllocates
                         error:(NSError **)error {
    using namespace ember::jit;
    @synchronized(self) {
        if (gCodeCache) return YES;
        setTraceHook(EMTraceLine);
        setDebuggerAllocates(debuggerAllocates);
        if (!processIsDebugged()) {
            EMTraceLine("prepare: process is not debugged, stopping");
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
            EMTraceLine([NSString stringWithFormat:@"prepare: FAILED - %s", reason.c_str()].UTF8String);
            return EMFail(error, [NSString stringWithFormat:@"Could not create the code cache: %s", reason.c_str()]);
        }
        if (!smokeTest(region)) {
            freeRegion(region);
            EMTraceLine("prepare: FAILED - the code cache did not execute correctly");
            return EMFail(error, @"The code cache was created but did not execute correctly.");
        }
        gCodeCache = region;
        EMTraceLine("prepare: SUCCESS - JIT memory works");
        return YES;
    }
}

+ (void)trace:(NSString *)message {
    EMTraceLine(message.UTF8String ?: "");
}

+ (NSURL *)traceURL {
    return [NSURL fileURLWithFileSystemRepresentation:EMTracePath() isDirectory:NO relativeToURL:nil];
}

+ (NSString *)traceText {
    NSString *text = [NSString stringWithContentsOfURL:self.traceURL encoding:NSUTF8StringEncoding error:nil] ?: @"";
    // Keep the screen readable: the most recent part is what matters.
    const NSUInteger limit = 6000;
    return text.length > limit ? [text substringFromIndex:text.length - limit] : text;
}

+ (void)clearTrace {
    [NSFileManager.defaultManager removeItemAtURL:self.traceURL error:nil];
}

+ (NSString *)deviceModel {
    char model[64] = {};
    size_t size = sizeof(model) - 1;
    if (sysctlbyname("hw.machine", model, &size, NULL, 0) != 0) return @"unknown";
    return @(model);
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
