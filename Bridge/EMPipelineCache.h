#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

/// Render-pipeline cache built to keep shader compilation off the frame.
///
/// Stutter on iOS comes from `newRenderPipelineState` running the Metal
/// compiler in the middle of a draw. This class attacks it three ways:
///   1. In-memory map: a pipeline is only ever built once per run.
///   2. `MTLBinaryArchive` on disk: compiled pipelines are reloaded on the next
///      launch without invoking the compiler (iOS invalidates the archive
///      itself on OS/GPU driver updates; a failed load just starts empty).
///   3. Asynchronous builds: a miss is compiled on a background queue and the
///      caller is told "not ready" so it can skip that draw instead of stalling.
///
/// The presenter uses it for its own pipelines; a GX2->Metal backend should
/// key entries on (vertex shader hash, pixel shader hash, render state hash).
NS_SWIFT_NAME(PipelineCache)
@interface EMPipelineCache : NSObject

- (instancetype)initWithDevice:(id<MTLDevice>)device
                    archiveURL:(nullable NSURL *)archiveURL NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Non-blocking. Returns the pipeline if it is ready; otherwise starts an
/// asynchronous build (once) and returns nil. `descriptor` is evaluated only
/// on a miss.
- (nullable id<MTLRenderPipelineState>)pipelineForKey:(NSString *)key
                                           descriptor:(MTLRenderPipelineDescriptor * (NS_NOESCAPE ^)(void))descriptor
    NS_SWIFT_NAME(pipeline(forKey:descriptor:));

/// Blocking build, for pipelines that must exist before the first frame and
/// for the boot-time "precompile shaders" pass.
- (nullable id<MTLRenderPipelineState>)buildPipelineForKey:(NSString *)key
                                                descriptor:(MTLRenderPipelineDescriptor *)descriptor
                                                     error:(NSError **)error
    NS_SWIFT_NAME(buildPipeline(forKey:descriptor:));

/// Writes newly compiled pipelines to the archive. Call on pause/background,
/// never per frame.
- (BOOL)flush:(NSError **)error;

/// Deletes the on-disk archive and forgets every pipeline.
- (void)removeAll;

@property (nonatomic, readonly) NSUInteger readyCount;
@property (nonatomic, readonly) NSUInteger pendingCount;

@end

NS_ASSUME_NONNULL_END
