import MetalKit
import UIKit
#if canImport(MetalFX) && !targetEnvironment(simulator)
import MetalFX
#endif

/// Mirrors `PresentUniforms` in MetalShaders/Present.metal (16 bytes).
private struct PresentUniforms {
    var rect: SIMD4<Float>
}

/// Mirrors `SharpenUniforms` in MetalShaders/PostProcess.metal.
private struct SharpenUniforms {
    var sharpness: Float
}

/// Takes the guest's frame at its internal resolution and gets it onto the
/// display:
///
///     guest frame (640×480 × render scale)
///        └─ FXAA (optional, compute, same size)
///             └─ upscale to the on-screen size
///                  · Bilinear        — the final pass's sampler does it
///                  · Sharpen         — compute: resample + adaptive sharpening
///                  · MetalFX Spatial — Apple's spatial upscaler (iOS 16+)
///                       └─ present, fitted to the chosen aspect ratio
///
/// Every pipeline is built once in `init`, before the first frame, so
/// switching scale or filter mid-game never compiles anything. Intermediate
/// textures are reallocated only when a size actually changes.
final class FrameRenderer: NSObject, MTKViewDelegate {
    let device: MTLDevice?

    // Set from SwiftUI; read on the main thread in draw.
    var upscaler: Upscaler = .bilinear
    var fxaa = false
    var sharpness: Float = 0.5
    var aspect: AspectMode = .original
    var renderScale: RenderScale = .native {
        didSet {
            guard renderScale != oldValue else { return }
            let scale = Float(renderScale.rawValue)
            MainActor.assumeIsolated { session.core.setRenderScale(scale) } // set from SwiftUI, i.e. the main thread
        }
    }

    private let session: EmulatorSession
    private let queue: MTLCommandQueue?
    private let pipelines: PipelineCache?
    private var presentPipeline: MTLRenderPipelineState?
    private var fxaaPipeline: MTLComputePipelineState?
    private var sharpenPipeline: MTLComputePipelineState?
    private var sampler: MTLSamplerState?

    private var fxaaTexture: MTLTexture?
    private var outputTexture: MTLTexture?
    private var spatialScaler: AnyObject? // MTLFXSpatialScaler; untyped so the class loads on iOS 15
    private var spatialScalerKey = ""
    private var backgroundObserver: NSObjectProtocol?

    init(session: EmulatorSession) {
        self.session = session
        device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        if let device = device {
            let archive = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("Ember/pipelines.metallib-archive")
            pipelines = PipelineCache(device: device, archiveURL: archive)
        } else {
            pipelines = nil
        }
        super.init()
        buildResources()
        // Save newly compiled pipelines whenever the app leaves the foreground;
        // iOS may terminate a suspended app without further notice.
        backgroundObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: nil
        ) { [weak self] _ in self?.flushPipelineCache() }
    }

    deinit {
        if let backgroundObserver = backgroundObserver { NotificationCenter.default.removeObserver(backgroundObserver) }
        flushPipelineCache()
    }

    func flushPipelineCache() {
        try? pipelines?.flush()
    }

    /// Whether the MetalFX option can do anything on this device.
    static var metalFXAvailable: Bool {
        #if canImport(MetalFX) && !targetEnvironment(simulator)
        if #available(iOS 16.0, *), let device = MTLCreateSystemDefaultDevice() {
            return MTLFXSpatialScalerDescriptor.supportsDevice(device)
        }
        #endif
        return false
    }

    private func buildResources() {
        guard let device = device, let library = device.makeDefaultLibrary() else { return }

        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.label = "Present"
        descriptor.vertexFunction = library.makeFunction(name: "present_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "present_fragment")
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        presentPipeline = try? pipelines?.buildPipeline(forKey: "present.bgra8", descriptor: descriptor)

        if let function = library.makeFunction(name: "fxaa_kernel") {
            fxaaPipeline = try? device.makeComputePipelineState(function: function)
        }
        if let function = library.makeFunction(name: "upscale_sharpen_kernel") {
            sharpenPipeline = try? device.makeComputePipelineState(function: function)
        }

        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.minFilter = .linear
        samplerDescriptor.magFilter = .linear
        samplerDescriptor.sAddressMode = .clampToEdge
        samplerDescriptor.tAddressMode = .clampToEdge
        sampler = device.makeSamplerState(descriptor: samplerDescriptor)
    }

    // MARK: Textures

    /// Returns `existing` if it already has the right size, otherwise a new texture.
    private func intermediate(_ existing: MTLTexture?, width: Int, height: Int, label: String) -> MTLTexture? {
        if let existing = existing, existing.width == width, existing.height == height { return existing }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = [.shaderRead, .shaderWrite, .renderTarget] // compute output, and a MetalFX target
        descriptor.storageMode = .private
        let texture = device?.makeTexture(descriptor: descriptor)
        texture?.label = label
        return texture
    }

    private func runCompute(_ pipeline: MTLComputePipelineState, on commands: MTLCommandBuffer, from source: MTLTexture,
                            to destination: MTLTexture, configure: (MTLComputeCommandEncoder) -> Void = { _ in }) {
        guard let encoder = commands.makeComputeCommandEncoder() else { return }
        encoder.setComputePipelineState(pipeline)
        encoder.setTexture(source, index: 0)
        encoder.setTexture(destination, index: 1)
        configure(encoder)
        let w = pipeline.threadExecutionWidth
        let h = max(1, pipeline.maxTotalThreadsPerThreadgroup / w)
        // Rounded up; the kernels discard threads that fall outside the texture.
        let groups = MTLSize(width: (destination.width + w - 1) / w, height: (destination.height + h - 1) / h, depth: 1)
        encoder.dispatchThreadgroups(groups, threadsPerThreadgroup: MTLSize(width: w, height: h, depth: 1))
        encoder.endEncoding()
    }

    /// Encodes a MetalFX spatial upscale, or returns false if it cannot be used
    /// (iOS 15, unsupported GPU, or the internal image is not smaller than the
    /// output — a spatial scaler only scales up).
    private func encodeMetalFX(on commands: MTLCommandBuffer, from source: MTLTexture, to destination: MTLTexture) -> Bool {
        #if canImport(MetalFX) && !targetEnvironment(simulator)
        guard #available(iOS 16.0, *), let device = device,
              source.width <= destination.width, source.height <= destination.height,
              MTLFXSpatialScalerDescriptor.supportsDevice(device) else { return false }

        let key = "\(source.width)x\(source.height)>\(destination.width)x\(destination.height)"
        if spatialScalerKey != key {
            let descriptor = MTLFXSpatialScalerDescriptor()
            descriptor.inputWidth = source.width
            descriptor.inputHeight = source.height
            descriptor.outputWidth = destination.width
            descriptor.outputHeight = destination.height
            descriptor.colorTextureFormat = source.pixelFormat
            descriptor.outputTextureFormat = destination.pixelFormat
            descriptor.colorProcessingMode = .perceptual // the guest's output is display-referred sRGB-like
            spatialScaler = descriptor.makeSpatialScaler(device: device)
            spatialScalerKey = key
        }
        guard let scaler = spatialScaler as? MTLFXSpatialScaler else { return false }
        scaler.colorTexture = source
        scaler.outputTexture = destination
        scaler.encode(commandBuffer: commands)
        return true
        #else
        return false
        #endif
    }

    // MARK: MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        // MTKView drives this on the main thread.
        MainActor.assumeIsolated { render(in: view) }
    }

    @MainActor
    private func render(in view: MTKView) {
        session.pushInput()

        guard let device = device, let presentPipeline = presentPipeline, let sampler = sampler,
              let drawable = view.currentDrawable, let pass = view.currentRenderPassDescriptor,
              let commands = queue?.makeCommandBuffer() else { return }
        commands.label = "Frame"

        // Where the picture goes on screen, in drawable pixels.
        let drawableSize = view.drawableSize
        var target = CGRect(origin: .zero, size: drawableSize)
        if let ratio = aspect.ratio, drawableSize.width > 0, drawableSize.height > 0 {
            var size = CGSize(width: drawableSize.width, height: drawableSize.width / ratio)
            if size.height > drawableSize.height { size = CGSize(width: drawableSize.height * ratio, height: drawableSize.height) }
            target = CGRect(x: (drawableSize.width - size.width) / 2, y: (drawableSize.height - size.height) / 2,
                            width: size.width, height: size.height)
        }
        let outWidth = max(1, Int(target.width.rounded())), outHeight = max(1, Int(target.height.rounded()))

        var image = session.core.frameTexture(device: device)
        var stages: [String] = []
        if let source = image {
            stages.append("\(source.width)×\(source.height) → \(outWidth)×\(outHeight)")

            if fxaa, let pipeline = fxaaPipeline {
                fxaaTexture = intermediate(fxaaTexture, width: source.width, height: source.height, label: "FXAA")
                if let destination = fxaaTexture {
                    runCompute(pipeline, on: commands, from: source, to: destination)
                    image = destination
                    stages.append("FXAA")
                }
            }

            if upscaler != .bilinear, let current = image {
                outputTexture = intermediate(outputTexture, width: outWidth, height: outHeight, label: "Upscaled")
                if let destination = outputTexture {
                    if upscaler == .metalFX, encodeMetalFX(on: commands, from: current, to: destination) {
                        image = destination
                        stages.append("MetalFX")
                    } else if upscaler == .sharpen, let pipeline = sharpenPipeline {
                        var uniforms = SharpenUniforms(sharpness: sharpness)
                        runCompute(pipeline, on: commands, from: current, to: destination) { encoder in
                            encoder.setBytes(&uniforms, length: MemoryLayout<SharpenUniforms>.stride, index: 0)
                        }
                        image = destination
                        stages.append("Sharpen")
                    }
                }
            }
            if stages.count == 1 || stages.last == "FXAA" { stages.append("Bilinear") }
        }
        // For the statistics overlay, e.g. "1440×1080 → 1560×1170 · MetalFX".
        session.renderInfo = stages.joined(separator: " · ")

        if let encoder = commands.makeRenderCommandEncoder(descriptor: pass) {
            if let image = image, drawableSize.width > 0, drawableSize.height > 0 {
                // Pixels (origin top-left, y down) -> NDC (origin centre, y up).
                var uniforms = PresentUniforms(rect: SIMD4(
                    Float(target.minX / drawableSize.width * 2 - 1), Float(1 - target.maxY / drawableSize.height * 2),
                    Float(target.maxX / drawableSize.width * 2 - 1), Float(1 - target.minY / drawableSize.height * 2)))
                encoder.setRenderPipelineState(presentPipeline)
                encoder.setVertexBytes(&uniforms, length: MemoryLayout<PresentUniforms>.stride, index: 0)
                encoder.setFragmentTexture(image, index: 0)
                encoder.setFragmentSamplerState(sampler, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        commands.present(drawable)
        commands.commit()
    }
}
