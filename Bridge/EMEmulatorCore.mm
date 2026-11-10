#import "EMEmulatorCore.h"

NSErrorDomain const EMEmulatorErrorDomain = @"app.ember.emulator";

static NSError *EMMakeError(NSInteger code, NSString *message) {
    return [NSError errorWithDomain:EMEmulatorErrorDomain
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey : message}];
}

// MARK: - Value objects

@interface EMTitleInfo ()
- (instancetype)initWithURL:(NSURL *)url info:(const em_title_info &)info;
@end

@implementation EMTitleInfo
- (instancetype)initWithURL:(NSURL *)url info:(const em_title_info &)info {
    if ((self = [super init])) {
        _url       = url;
        _format    = (EMTitleFormat)info.format;
        _verified  = info.verified;
        _titleID   = info.title_id;
        _titleCode = [NSString stringWithUTF8String:info.title_code] ?: @"";
        _region    = info.region;
        _sizeBytes = info.size_bytes;
        _name      = [NSString stringWithUTF8String:info.name] ?: url.lastPathComponent;
    }
    return self;
}
@end

@implementation EMConfiguration
- (instancetype)init {
    if ((self = [super init])) {
        _cpuMode      = EMCPUModeInterpreter;
        _ramMB        = 64;
        _renderScale  = 1.0f;
        _vsync        = YES;
        _asyncShaders = YES;
        NSFileManager *fm = NSFileManager.defaultManager;
        _dataDirectory  = [[fm URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject
            URLByAppendingPathComponent:@"Ember" isDirectory:YES];
        _cacheDirectory = [[fm URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject
            URLByAppendingPathComponent:@"Ember" isDirectory:YES];
    }
    return self;
}
@end

@interface EMStats ()
- (instancetype)initWithStats:(const em_stats &)stats;
@end

@implementation EMStats
- (instancetype)initWithStats:(const em_stats &)s {
    if ((self = [super init])) {
        _fps              = s.fps;
        _guestMIPS        = s.guest_mips;
        _ramMB            = s.ram_mb;
        _frameWidth       = s.frame_width;
        _frameHeight      = s.frame_height;
        _recompilerActive = s.recompiler_active;
        _status           = [NSString stringWithUTF8String:s.status] ?: @"";
    }
    return self;
}
@end

// MARK: - Core

@implementation EMEmulatorCore {
    em_core        *_core;
    id<MTLTexture>  _texture;
    uint64_t        _uploadedSerial;
}

+ (NSString *)backendName {
    return @(em_core_backend_name());
}

+ (nullable EMTitleInfo *)probeTitleAtURL:(NSURL *)url {
    em_title_info info;
    if (!em_probe_title(url.fileSystemRepresentation, &info)) return nil;
    return [[EMTitleInfo alloc] initWithURL:url info:info];
}

- (nullable instancetype)initWithConfiguration:(EMConfiguration *)configuration error:(NSError **)error {
    if (!(self = [super init])) return nil;

    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSURL *dir in @[configuration.dataDirectory, configuration.cacheDirectory]) {
        [fm createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
    }

    em_config cfg = {};
    cfg.cpu_mode      = (em_cpu_mode)configuration.cpuMode;
    cfg.ram_mb        = configuration.ramMB;
    cfg.render_scale  = configuration.renderScale;
    cfg.vsync         = configuration.vsync;
    cfg.async_shaders = configuration.asyncShaders;
    cfg.data_dir      = configuration.dataDirectory.fileSystemRepresentation;
    cfg.cache_dir     = configuration.cacheDirectory.fileSystemRepresentation;

    _core = em_core_create(&cfg);
    if (!_core) {
        if (error) *error = EMMakeError(1, @"The core could not be created. Guest RAM must be 64 or 128 MB.");
        return nil;
    }
    return self;
}

- (void)dealloc {
    em_core_destroy(_core);
}

- (em_core *)handle {
    return _core;
}

- (BOOL)loadTitleAtURL:(NSURL *)url error:(NSError **)error {
    char message[256] = {};
    if (em_core_load_title(_core, url.fileSystemRepresentation, message, sizeof(message))) return YES;
    if (error) *error = EMMakeError(2, @(message));
    return NO;
}

- (void)start { em_core_start(_core); }
- (void)setPaused:(BOOL)paused { em_core_set_paused(_core, paused); }
- (void)stop { em_core_stop(_core); }

- (void)setPadState:(em_pad_state)state port:(NSInteger)port {
    em_core_set_pad(_core, (int)port, &state);
}

- (void)setRenderScale:(float)scale {
    em_core_set_render_scale(_core, scale);
}

- (nullable id<MTLTexture>)frameTextureForDevice:(id<MTLDevice>)device {
    const uint8_t *pixels = nullptr;
    uint32_t width = 0, height = 0;
    uint64_t serial = 0;
    if (!em_core_lock_frame(_core, &pixels, &width, &height, &serial)) return nil;
    if (serial == 0 || width == 0 || height == 0) {
        em_core_unlock_frame(_core);
        return nil;
    }

    // The internal resolution changes when the render scale is switched; the
    // texture is simply recreated at the new size.
    if (!_texture || _texture.width != width || _texture.height != height) {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        desc.usage       = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        _texture = [device newTextureWithDescriptor:desc];
        _texture.label = @"Guest framebuffer";
        _uploadedSerial = 0;
    }
    if (serial != _uploadedSerial) {
        [_texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                    mipmapLevel:0
                      withBytes:pixels
                    bytesPerRow:(NSUInteger)width * 4];
        _uploadedSerial = serial;
    }
    em_core_unlock_frame(_core);
    return _texture;
}

- (EMStats *)stats {
    em_stats s = {};
    em_core_get_stats(_core, &s);
    return [[EMStats alloc] initWithStats:s];
}

@end
