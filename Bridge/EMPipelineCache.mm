#import "EMPipelineCache.h"

#import <os/lock.h>

@implementation EMPipelineCache {
    id<MTLDevice>        _device;
    NSURL               *_archiveURL;
    id<MTLBinaryArchive> _archive;
    BOOL                 _archiveDirty;
    os_unfair_lock       _lock;
    NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *_ready;
    NSMutableSet<NSString *> *_pending;
    dispatch_queue_t     _compileQueue; // concurrent: Metal compiles off the render thread
    dispatch_queue_t     _archiveQueue; // serial: all writes to the binary archive
}

- (instancetype)initWithDevice:(id<MTLDevice>)device archiveURL:(nullable NSURL *)archiveURL {
    if ((self = [super init])) {
        _device     = device;
        _archiveURL = archiveURL;
        _lock       = OS_UNFAIR_LOCK_INIT;
        _ready      = [NSMutableDictionary dictionary];
        _pending    = [NSMutableSet set];
        _compileQueue = dispatch_queue_create("app.ember.pipeline.compile",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
        _archiveQueue = dispatch_queue_create("app.ember.pipeline.archive",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
        [self openArchive];
    }
    return self;
}

- (void)openArchive {
    if (!_archiveURL) return;
    MTLBinaryArchiveDescriptor *desc = [MTLBinaryArchiveDescriptor new];
    if ([NSFileManager.defaultManager fileExistsAtPath:_archiveURL.path]) desc.url = _archiveURL;
    NSError *error = nil;
    _archive = [_device newBinaryArchiveWithDescriptor:desc error:&error];
    if (!_archive) {
        // Stale or corrupt (e.g. after an iOS update): start a fresh one.
        [NSFileManager.defaultManager removeItemAtURL:_archiveURL error:nil];
        _archive = [_device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:nil];
    }
}

// Looking a pipeline up in the archive is free; *adding* one compiles it, so
// recording always happens on the archive queue, after the pipeline is in use.
- (void)recordInArchive:(MTLRenderPipelineDescriptor *)descriptor {
    id<MTLBinaryArchive> archive = _archive;
    if (!archive) return;
    MTLRenderPipelineDescriptor *copy = [descriptor copy];
    copy.binaryArchives = nil;
    dispatch_async(_archiveQueue, ^{
        if ([archive addRenderPipelineFunctionsWithDescriptor:copy error:nil]) {
            os_unfair_lock_lock(&self->_lock);
            self->_archiveDirty = YES;
            os_unfair_lock_unlock(&self->_lock);
        }
    });
}

- (nullable id<MTLRenderPipelineState>)compile:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    if (_archive) descriptor.binaryArchives = @[_archive];
    id<MTLRenderPipelineState> state = [_device newRenderPipelineStateWithDescriptor:descriptor error:error];
    if (state) [self recordInArchive:descriptor];
    return state;
}

- (nullable id<MTLRenderPipelineState>)pipelineForKey:(NSString *)key
                                           descriptor:(MTLRenderPipelineDescriptor * (NS_NOESCAPE ^)(void))descriptorBlock {
    os_unfair_lock_lock(&_lock);
    id<MTLRenderPipelineState> hit = _ready[key];
    const BOOL alreadyBuilding = [_pending containsObject:key];
    if (!hit && !alreadyBuilding) [_pending addObject:key];
    os_unfair_lock_unlock(&_lock);
    if (hit || alreadyBuilding) return hit;

    MTLRenderPipelineDescriptor *descriptor = descriptorBlock();
    dispatch_async(_compileQueue, ^{
        NSError *error = nil;
        id<MTLRenderPipelineState> state = [self compile:descriptor error:&error];
        os_unfair_lock_lock(&self->_lock);
        [self->_pending removeObject:key];
        if (state) self->_ready[key] = state;
        os_unfair_lock_unlock(&self->_lock);
        if (!state) NSLog(@"[PipelineCache] %@ failed: %@", key, error.localizedDescription);
    });
    return nil;
}

- (nullable id<MTLRenderPipelineState>)buildPipelineForKey:(NSString *)key
                                                descriptor:(MTLRenderPipelineDescriptor *)descriptor
                                                     error:(NSError **)error {
    os_unfair_lock_lock(&_lock);
    id<MTLRenderPipelineState> hit = _ready[key];
    os_unfair_lock_unlock(&_lock);
    if (hit) return hit;

    id<MTLRenderPipelineState> state = [self compile:descriptor error:error];
    if (!state) return nil;
    os_unfair_lock_lock(&_lock);
    _ready[key] = state;
    os_unfair_lock_unlock(&_lock);
    return state;
}

- (BOOL)flush:(NSError **)error {
    if (!_archive || !_archiveURL) return YES;
    __block BOOL ok = YES;
    __block NSError *failure = nil;
    dispatch_sync(_archiveQueue, ^{ // drains pending adds first
        os_unfair_lock_lock(&self->_lock);
        const BOOL dirty = self->_archiveDirty;
        self->_archiveDirty = NO;
        os_unfair_lock_unlock(&self->_lock);
        if (!dirty) return;
        [NSFileManager.defaultManager createDirectoryAtURL:self->_archiveURL.URLByDeletingLastPathComponent
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:nil];
        NSError *local = nil;
        ok = [self->_archive serializeToURL:self->_archiveURL error:&local];
        failure = local;
    });
    if (!ok && error) *error = failure;
    return ok;
}

- (void)removeAll {
    os_unfair_lock_lock(&_lock);
    [_ready removeAllObjects];
    _archiveDirty = NO;
    os_unfair_lock_unlock(&_lock);
    if (_archiveURL) [NSFileManager.defaultManager removeItemAtURL:_archiveURL error:nil];
    _archive = nil;
    [self openArchive];
}

- (NSUInteger)readyCount {
    os_unfair_lock_lock(&_lock);
    const NSUInteger n = _ready.count;
    os_unfair_lock_unlock(&_lock);
    return n;
}

- (NSUInteger)pendingCount {
    os_unfair_lock_lock(&_lock);
    const NSUInteger n = _pending.count;
    os_unfair_lock_unlock(&_lock);
    return n;
}

@end
