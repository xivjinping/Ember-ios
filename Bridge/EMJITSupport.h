#import <Foundation/Foundation.h>

@class EMEmulatorCore;

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, EMTXMStatus) {
    EMTXMStatusUnknown = 0,
    EMTXMStatusAbsent  = 1,
    EMTXMStatusPresent = 2,
} NS_SWIFT_NAME(TXMStatus);

/// Platform half of the JIT story: what this install is allowed to do, and
/// the one call that turns a debugged process into one with a code cache.
/// The step-by-step flow that drives these calls is Frontend/App/JITCoordinator.swift
/// and is explained in docs/JIT.md.
NS_SWIFT_NAME(JITSupport)
@interface EMJITSupport : NSObject

/// The signature carries `get-task-allow`. Without it no debugger can attach
/// and JIT is impossible for this install (re-sign with a development profile).
@property (class, nonatomic, readonly) BOOL hasGetTaskAllow;

/// A debugger has attached at least once (CS_DEBUGGED). Sticky until exit.
@property (class, nonatomic, readonly) BOOL isDebugged;

/// Whether the device runs the Trusted Execution Monitor. On iOS 26+ this
/// decides between "debugger attach is enough" and the breakpoint protocol.
@property (class, nonatomic, readonly) EMTXMStatus txmStatus;

/// Running inside LiveContainer (different bundle-ID handling for enablers).
@property (class, nonatomic, readonly) BOOL isInsideLiveContainer;

/// Bytes the app may still allocate before jetsam (os_proc_available_memory).
@property (class, nonatomic, readonly) uint64_t availableMemoryBytes;
@property (class, nonatomic, readonly) uint64_t physicalMemoryBytes;

/// Allocates the process-wide dual-mapped code cache, completes the TXM
/// handshake if `useTXMProtocol` is set, and smoke-tests the result. The
/// cache is created once per process and reused by every game session:
/// with TXM, regions cannot be added after the enabler's script detaches.
///
/// Blocking: with the TXM protocol each step waits on the debugger script.
/// Call off the main thread, only when `isDebugged` is YES, and with the TXM
/// protocol only while the enabler's script is attached. Returns YES
/// immediately if the cache already exists.
///
/// `debuggerAllocates` (TXM protocol only) lets the debug server allocate the
/// region instead of registering one this process mapped.
+ (BOOL)prepareCodeCacheOfSize:(NSUInteger)bytes
                useTXMProtocol:(BOOL)useTXMProtocol
             debuggerAllocates:(BOOL)debuggerAllocates
                         error:(NSError **)error
    NS_SWIFT_NAME(prepareCodeCache(bytes:useTXMProtocol:debuggerAllocates:));

// MARK: Diagnostics

/// JIT setup tends to fail by killing the app, so every step is written to
/// Documents/Ember/jit-trace.txt *before* it runs. After a crash, the last
/// line names the step that died. The file is also visible in the Files app.
+ (void)trace:(NSString *)message NS_SWIFT_NAME(trace(_:));
@property (class, nonatomic, readonly, copy) NSString *traceText;
@property (class, nonatomic, readonly) NSURL *traceURL;
+ (void)clearTrace;

/// Hardware identifier, e.g. "iPad14,8".
@property (class, nonatomic, readonly, copy) NSString *deviceModel;

@property (class, nonatomic, readonly) BOOL isCodeCacheReady;

/// Hands the prepared cache to a core. Returns NO if there is none (the core
/// then stays on the interpreter).
+ (BOOL)attachCodeCacheToCore:(EMEmulatorCore *)core NS_SWIFT_NAME(attachCodeCache(to:));

@end

NS_ASSUME_NONNULL_END
