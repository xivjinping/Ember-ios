#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#import "ember/core_api.h"

NS_ASSUME_NONNULL_BEGIN

extern NSErrorDomain const EMEmulatorErrorDomain;

typedef NS_ENUM(NSInteger, EMTitleFormat) {
    EMTitleFormatUnknown = EM_TITLE_UNKNOWN,
    EMTitleFormatXBE     = EM_TITLE_XBE,
    EMTitleFormatXISO    = EM_TITLE_XISO,
    EMTitleFormatFolder  = EM_TITLE_FOLDER,
} NS_SWIFT_NAME(TitleFormat);

typedef NS_ENUM(NSInteger, EMCPUMode) {
    EMCPUModeInterpreter = EM_CPU_INTERPRETER,
    EMCPUModeRecompiler  = EM_CPU_RECOMPILER,
} NS_SWIFT_NAME(CPUMode);

/// Result of probing one file or folder for the game library.
NS_SWIFT_NAME(TitleInfo)
@interface EMTitleInfo : NSObject
@property (nonatomic, readonly) NSURL *url;
@property (nonatomic, readonly) EMTitleFormat format;
/// NO when only the file extension matched: the image could not be parsed as an Xbox title.
@property (nonatomic, readonly) BOOL verified;
/// From the executable's certificate; 0 if unknown.
@property (nonatomic, readonly) uint32_t titleID;
/// Publisher/number form of the ID, e.g. "MS-004"; empty if unknown.
@property (nonatomic, readonly, copy) NSString *titleCode;
@property (nonatomic, readonly) uint32_t region;
@property (nonatomic, readonly) uint64_t sizeBytes;
@property (nonatomic, readonly, copy) NSString *name;
- (instancetype)init NS_UNAVAILABLE;
@end

NS_SWIFT_NAME(CoreConfiguration)
@interface EMConfiguration : NSObject
@property (nonatomic) EMCPUMode cpuMode;
/// 64 or 128. Anything else makes core creation fail.
@property (nonatomic) uint32_t ramMB;
@property (nonatomic) float renderScale;
@property (nonatomic) BOOL vsync;
@property (nonatomic) BOOL asyncShaders;
@property (nonatomic) BOOL legacyDisplay;
@property (nonatomic, copy) NSURL *dataDirectory;
@property (nonatomic, copy) NSURL *cacheDirectory;
@end

NS_SWIFT_NAME(CoreStats)
@interface EMStats : NSObject
@property (nonatomic, readonly) double fps;
@property (nonatomic, readonly) double guestMIPS;
@property (nonatomic, readonly) uint32_t ramMB;
@property (nonatomic, readonly) uint32_t frameWidth;
@property (nonatomic, readonly) uint32_t frameHeight;
@property (nonatomic, readonly) BOOL recompilerActive;
/// What the CPU is doing; with the stub backend, where execution stopped.
@property (nonatomic, readonly, copy) NSString *status;
@end

/// One running emulator instance. Thread-safe unless noted.
NS_SWIFT_NAME(EmulatorCore)
@interface EMEmulatorCore : NSObject

/// "stub" until a real engine is linked; shown in the UI so nobody mistakes
/// the test pattern for a broken game.
@property (class, nonatomic, readonly) NSString *backendName;

/// Opens the image: reads the XDVDFS volume and the executable's certificate.
+ (nullable EMTitleInfo *)probeTitleAtURL:(NSURL *)url NS_SWIFT_NAME(probeTitle(at:));

- (nullable instancetype)initWithConfiguration:(EMConfiguration *)configuration
                                         error:(NSError **)error NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

- (BOOL)loadTitleAtURL:(NSURL *)url error:(NSError **)error NS_SWIFT_NAME(loadTitle(at:));
- (void)start;
- (void)setPaused:(BOOL)paused;
- (void)stop;

/// Raw handle for the audio render callback, which must not message Obj-C.
/// Valid until -stop / dealloc.
@property (nonatomic, readonly) em_core *handle;

- (void)setPadState:(em_pad_state)state port:(NSInteger)port NS_SWIFT_NAME(setPad(_:port:));

/// Internal resolution multiplier; can be changed while a title is running.
- (void)setRenderScale:(float)scale;

/// Latest frame at the internal resolution as a BGRA8 texture, or nil before
/// the first frame. Call from the render thread only. The stub uploads from
/// CPU memory; a GPU backend returns its own render target with no copy.
- (nullable id<MTLTexture>)frameTextureForDevice:(id<MTLDevice>)device NS_SWIFT_NAME(frameTexture(device:));

- (EMStats *)stats;

@end

NS_ASSUME_NONNULL_END
