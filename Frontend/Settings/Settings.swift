import Foundation
import SwiftUI

/// Internal rendering resolution as a multiple of the console's 640×480.
enum RenderScale: Double, CaseIterable, Identifiable {
    case native = 1.0, hd720 = 1.5, hd1080 = 2.25, qhd1440 = 3.0, uhd4K = 4.5
    var id: Double { rawValue }
    var width: Int { Int((640 * rawValue).rounded()) }
    var height: Int { Int((480 * rawValue).rounded()) }
    var title: String {
        switch self {
        case .native: return "1× · 480p (native)"
        case .hd720: return "1.5× · 720p"
        case .hd1080: return "2.25× · 1080p"
        case .qhd1440: return "3× · 1440p"
        case .uhd4K: return "4.5× · 4K"
        }
    }
    var shortTitle: String {
        switch self {
        case .native: return "480p"
        case .hd720: return "720p"
        case .hd1080: return "1080p"
        case .qhd1440: return "1440p"
        case .uhd4K: return "4K"
        }
    }
}

/// How the internal image is brought to the display's resolution.
enum Upscaler: String, CaseIterable, Identifiable {
    case bilinear, sharpen, metalFX
    var id: String { rawValue }
    var title: String {
        switch self {
        case .bilinear: return "Bilinear"
        case .sharpen: return "Bilinear + adaptive sharpening"
        case .metalFX: return "MetalFX Spatial (FSR-class, iOS 16+)"
        }
    }
    var shortTitle: String {
        switch self {
        case .bilinear: return "Bilinear"
        case .sharpen: return "Sharpen"
        case .metalFX: return "MetalFX"
        }
    }
}

enum AspectMode: String, CaseIterable, Identifiable {
    case original, widescreen, stretch
    var id: String { rawValue }
    var title: String {
        switch self {
        case .original: return "4:3 (original)"
        case .widescreen: return "16:9 (for games set to widescreen)"
        case .stretch: return "Fill the screen"
        }
    }
    /// nil means "use the view's own aspect".
    var ratio: CGFloat? {
        switch self {
        case .original: return 4.0 / 3.0
        case .widescreen: return 16.0 / 9.0
        case .stretch: return nil
        }
    }
}

/// Where the Black and White buttons sit on the touch overlay.
enum ControllerLayout: String, CaseIterable, Identifiable {
    case controllerS, duke
    var id: String { rawValue }
    var title: String { self == .duke ? "Duke (Black/White above the face buttons)" : "Controller S (Black/White below)" }
}

/// Which memory-setup protocol to use once a debugger is attached.
enum JITProtocol: String, CaseIterable, Identifiable {
    case automatic, script, plain
    var id: String { rawValue }
    var title: String {
        switch self {
        case .automatic: return "Automatic (detect TXM)"
        case .script: return "Always use the JIT script"
        case .plain: return "Never use the JIT script"
        }
    }
}

enum JITMethod: String, CaseIterable, Identifiable {
    case stikDebug, waitForDebugger
    var id: String { rawValue }
    var title: String {
        switch self {
        case .stikDebug: return "StikDebug (on device)"
        case .waitForDebugger: return "Wait for debugger (AltJIT, SideJITServer, Xcode)"
        }
    }
}

/// User settings, persisted in UserDefaults. A plain ObservableObject rather
/// than @AppStorage so the same values can be read outside the view tree
/// (session setup, renderer) and the app still targets iOS 15.
final class AppSettings: ObservableObject {
    private let defaults: UserDefaults

    // CPU
    @Published var preferRecompiler: Bool { didSet { defaults.set(preferRecompiler, forKey: "cpu.recompiler") } }
    @Published var jitMethod: JITMethod { didSet { defaults.set(jitMethod.rawValue, forKey: "cpu.jitMethod") } }
    /// Overrides TXM detection, for when it is wrong or cannot tell.
    @Published var jitProtocol: JITProtocol { didSet { defaults.set(jitProtocol.rawValue, forKey: "cpu.jitProtocol") } }
    /// With the script: let the debug server allocate the region instead of registering ours.
    @Published var jitDebuggerAllocates: Bool { didSet { defaults.set(jitDebuggerAllocates, forKey: "cpu.jitDebuggerAllocates") } }
    @Published var codeCacheMB: Int { didSet { defaults.set(codeCacheMB, forKey: "cpu.codeCacheMB") } }

    // Memory
    @Published var extendedRAM: Bool { didSet { defaults.set(extendedRAM, forKey: "mem.128") } }

    // Graphics
    @Published var renderScale: RenderScale { didSet { defaults.set(renderScale.rawValue, forKey: "gfx.scale") } }
    @Published var fxaa: Bool { didSet { defaults.set(fxaa, forKey: "gfx.fxaa") } }
    @Published var upscaler: Upscaler { didSet { defaults.set(upscaler.rawValue, forKey: "gfx.upscaler") } }
    @Published var sharpness: Double { didSet { defaults.set(sharpness, forKey: "gfx.sharpness") } }
    @Published var aspect: AspectMode { didSet { defaults.set(aspect.rawValue, forKey: "gfx.aspect") } }
    @Published var vsync: Bool { didSet { defaults.set(vsync, forKey: "gfx.vsync") } }
    @Published var allow120Hz: Bool { didSet { defaults.set(allow120Hz, forKey: "gfx.120hz") } }
    @Published var asyncShaders: Bool { didSet { defaults.set(asyncShaders, forKey: "gfx.asyncShaders") } }
    @Published var precompileShaders: Bool { didSet { defaults.set(precompileShaders, forKey: "gfx.precompile") } }

    // Input and interface
    @Published var controllerLayout: ControllerLayout { didSet { defaults.set(controllerLayout.rawValue, forKey: "input.layout") } }
    @Published var touchControlOpacity: Double { didSet { defaults.set(touchControlOpacity, forKey: "input.touchOpacity") } }
    @Published var haptics: Bool { didSet { defaults.set(haptics, forKey: "input.haptics") } }
    @Published var showStats: Bool { didSet { defaults.set(showStats, forKey: "ui.stats") } }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let physicalMB = Int(JITSupport.physicalMemoryBytes / 1_048_576)
        defaults.register(defaults: [
            "cpu.recompiler": true,
            "cpu.jitMethod": JITMethod.stikDebug.rawValue,
            "cpu.codeCacheMB": 64,
            // Older and smaller devices start at 720p; everything else at 1080p.
            "gfx.scale": physicalMB < 5000 ? RenderScale.hd720.rawValue : RenderScale.hd1080.rawValue,
            "gfx.fxaa": false,
            "gfx.upscaler": Upscaler.bilinear.rawValue,
            "gfx.sharpness": 0.5,
            "gfx.aspect": AspectMode.original.rawValue,
            "gfx.vsync": true,
            "gfx.asyncShaders": true,
            "gfx.precompile": true,
            "input.layout": ControllerLayout.controllerS.rawValue,
            "input.touchOpacity": 0.6,
            "input.haptics": true,
        ])
        preferRecompiler = defaults.bool(forKey: "cpu.recompiler")
        jitMethod = JITMethod(rawValue: defaults.string(forKey: "cpu.jitMethod") ?? "") ?? .stikDebug
        jitProtocol = JITProtocol(rawValue: defaults.string(forKey: "cpu.jitProtocol") ?? "") ?? .automatic
        jitDebuggerAllocates = defaults.bool(forKey: "cpu.jitDebuggerAllocates")
        codeCacheMB = defaults.integer(forKey: "cpu.codeCacheMB")
        extendedRAM = defaults.bool(forKey: "mem.128")
        renderScale = RenderScale(rawValue: defaults.double(forKey: "gfx.scale")) ?? .hd720
        fxaa = defaults.bool(forKey: "gfx.fxaa")
        upscaler = Upscaler(rawValue: defaults.string(forKey: "gfx.upscaler") ?? "") ?? .bilinear
        sharpness = defaults.double(forKey: "gfx.sharpness")
        aspect = AspectMode(rawValue: defaults.string(forKey: "gfx.aspect") ?? "") ?? .original
        vsync = defaults.bool(forKey: "gfx.vsync")
        allow120Hz = defaults.bool(forKey: "gfx.120hz")
        asyncShaders = defaults.bool(forKey: "gfx.asyncShaders")
        precompileShaders = defaults.bool(forKey: "gfx.precompile")
        controllerLayout = ControllerLayout(rawValue: defaults.string(forKey: "input.layout") ?? "") ?? .controllerS
        touchControlOpacity = defaults.double(forKey: "input.touchOpacity")
        haptics = defaults.bool(forKey: "input.haptics")
        showStats = defaults.bool(forKey: "ui.stats")
    }

    /// The console's whole memory, CPU and GPU together. Fixed, so unlike
    /// most emulators there is no cap to tune: the guest cannot outgrow it.
    var ramMB: Int { extendedRAM ? 128 : 64 }

    func makeCoreConfiguration() -> CoreConfiguration {
        let config = CoreConfiguration()
        config.cpuMode = preferRecompiler ? .recompiler : .interpreter
        config.ramMB = UInt32(ramMB)
        config.renderScale = Float(renderScale.rawValue)
        config.vsync = vsync
        config.asyncShaders = asyncShaders
        return config
    }
}
