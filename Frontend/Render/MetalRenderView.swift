import MetalKit
import SwiftUI

/// The primary render view: an MTKView driven by `FrameRenderer`.
///
/// Resolution scale, anti-aliasing, upscaler and aspect ratio are plain
/// properties, so changing any of them in Settings or from the in-game menu
/// takes effect on the next frame without restarting the title.
struct MetalRenderView: UIViewRepresentable {
    let session: EmulatorSession
    var renderScale: RenderScale
    var fxaa: Bool
    var upscaler: Upscaler
    var sharpness: Double
    var aspect: AspectMode
    var allow120Hz: Bool

    func makeCoordinator() -> FrameRenderer { FrameRenderer(session: session) }

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: context.coordinator.device)
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.isOpaque = true
        view.isUserInteractionEnabled = false // the touch overlay above handles input
        view.delegate = context.coordinator
        return view
    }

    func updateUIView(_ view: MTKView, context: Context) {
        let renderer = context.coordinator
        renderer.renderScale = renderScale
        renderer.fxaa = fxaa
        renderer.upscaler = upscaler
        renderer.sharpness = Float(sharpness)
        renderer.aspect = aspect
        // Frame pacing: the display link fires at the panel's rate; the guest
        // produces 60 (NTSC) or 50 (PAL) frames a second regardless. 120 Hz
        // only shortens the wait between a finished frame and its present on
        // ProMotion panels (CADisableMinimumFrameDurationOnPhone is set in Info.plist).
        view.preferredFramesPerSecond = allow120Hz ? UIScreen.main.maximumFramesPerSecond : 60
    }
}
