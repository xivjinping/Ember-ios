import SwiftUI

@MainActor
struct SettingsView: View {
    @EnvironmentObject private var settings: AppSettings
    @EnvironmentObject private var jit: JITCoordinator
    @EnvironmentObject private var controllers: ControllerManager
    @Environment(\.dismiss) private var dismiss
    @State private var acquiring = false
    @State private var logRevision = 0
    @State private var editingLayout = false

    var body: some View {
        NavigationView {
            Form {
                graphicsSection
                cpuSection
                jitLogSection
                memorySection
                inputSection
                Section {
                    LabeledRow("Core backend", EmulatorCore.backendName)
                } footer: {
                    Text("Ember includes no games, BIOS, keys or system files. Use images of discs you own.")
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        .navigationViewStyle(.stack)
        .fullScreenCover(isPresented: $editingLayout) {
            PadLayoutEditorScreen().environmentObject(settings)
        }
    }

    // MARK: Graphics

    private var graphicsSection: some View {
        Section {
            Picker("Internal resolution", selection: $settings.renderScale) {
                ForEach(RenderScale.allCases) { Text($0.title).tag($0) }
            }
            Picker("Upscaler", selection: $settings.upscaler) {
                ForEach(Upscaler.allCases) { Text($0.title).tag($0) }
            }
            if settings.upscaler == .sharpen {
                VStack(alignment: .leading) {
                    Text("Sharpness: \(Int(settings.sharpness * 100))%")
                    Slider(value: $settings.sharpness, in: 0...1)
                }
            }
            if settings.upscaler == .metalFX && !FrameRenderer.metalFXAvailable {
                Text("MetalFX is not available on this device or iOS version; bilinear will be used.")
                    .font(.footnote).foregroundColor(.orange)
            }
            Toggle("FXAA anti-aliasing", isOn: $settings.fxaa)
            Picker("Aspect ratio", selection: $settings.aspect) {
                ForEach(AspectMode.allCases) { Text($0.title).tag($0) }
            }
            Toggle("V-Sync (pace to the console's refresh)", isOn: $settings.vsync)
            Toggle("Present at 120 Hz", isOn: $settings.allow120Hz)
            Toggle("Asynchronous shader compilation", isOn: $settings.asyncShaders)
            Toggle("Precompile cached shaders at boot", isOn: $settings.precompileShaders)
            Button("Clear Shader & Pipeline Caches", role: .destructive, action: clearCaches)
        } header: {
            Text("Graphics")
        } footer: {
            Text("Resolution, upscaler, FXAA and aspect can also be changed while a game is running. "
                 + "Higher internal resolution makes the picture sharper and costs GPU time and battery; "
                 + "turn on Show speed to see what your device keeps up with. "
                 + "iOS always presents in sync with the display, so V-Sync here controls guest pacing.")
        }
    }

    private func clearCaches() {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        try? FileManager.default.removeItem(at: caches.appendingPathComponent("Ember"))
    }

    // MARK: CPU

    private var cpuSection: some View {
        Section {
            Toggle("Use recompiler (JIT)", isOn: $settings.preferRecompiler)
            LabeledRow("JIT status", jit.statusText)
            LabeledRow("get-task-allow", JITSupport.hasGetTaskAllow ? "Present" : "Missing")
            LabeledRow("TXM", txmText)

            Picker("JIT method", selection: $settings.jitMethod) {
                ForEach(JITMethod.allCases) { Text($0.title).tag($0) }
            }
            Picker("JIT protocol", selection: $settings.jitProtocol) {
                ForEach(JITProtocol.allCases) { Text($0.title).tag($0) }
            }
            Toggle("Let StikDebug allocate the memory", isOn: $settings.jitDebuggerAllocates)
            Stepper("Code cache: \(settings.codeCacheMB) MB", value: $settings.codeCacheMB, in: 32...256, step: 32)
                .disabled(JITSupport.isCodeCacheReady)

            Button(action: enableJIT) {
                HStack {
                    Text("Enable JIT Now")
                    if acquiring { Spacer(); ProgressView() }
                }
            }
            .disabled(acquiring || jit.state == .ready || !JITSupport.hasGetTaskAllow)
        } header: {
            Text("CPU")
        } footer: {
            Text("Without JIT the interpreter is used, which is far too slow for games. "
                 + "“JIT protocol” overrides TXM detection if it is wrong. "
                 + (JITSupport.isInsideLiveContainer
                    ? "Running inside LiveContainer: enable “Use LiveContainer’s Bundle ID” there." : ""))
        }
    }

    // MARK: JIT log

    private var jitLogSection: some View {
        Section {
            let log = JITSupport.traceText
            if log.isEmpty {
                Text("No JIT attempts recorded yet.").foregroundColor(.secondary)
            } else {
                Text(log).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                Button("Copy Log") { UIPasteboard.general.string = log }
                Button("Clear Log", role: .destructive) {
                    JITSupport.clearTrace()
                    logRevision += 1
                }
            }
        } header: {
            Text("JIT log")
        } footer: {
            Text("Each step is recorded before it runs. If the app closed during a JIT attempt, the last line shows "
                 + "where. Also saved as Ember › jit-trace.txt in the Files app.")
        }
        .id(logRevision)
    }

    private var txmText: String {
        switch JITSupport.txmStatus {
        case .present: return "Present"
        case .absent: return "Not present"
        default: return "Unknown"
        }
    }

    private func enableJIT() {
        acquiring = true
        Task {
            _ = await jit.acquire(settings: settings)
            acquiring = false
        }
    }

    // MARK: Memory

    private var memorySection: some View {
        Section {
            Picker("Console RAM", selection: $settings.extendedRAM) {
                Text("64 MB (retail)").tag(false)
                Text("128 MB (upgraded)").tag(true)
            }
            LabeledRow("Device memory", "\(Int(JITSupport.physicalMemoryBytes / 1_048_576)) MB")
            LabeledRow("Available to this app", availableText)
        } header: {
            Text("Memory")
        } footer: {
            Text("The console's entire memory is this one fixed block, shared by its CPU and GPU, so the guest "
                 + "cannot grow past it. 128 MB matches upgraded consoles; a few titles misbehave with it. "
                 + "Takes effect the next time a game starts.")
        }
    }

    private var availableText: String {
        let mb = Int(JITSupport.availableMemoryBytes / 1_048_576)
        return mb > 0 ? "\(mb) MB" : "—"
    }

    // MARK: Input

    private var inputSection: some View {
        Section {
            LabeledRow("Controller", controllers.controllerName ?? "None — touch controls shown")
            Picker("Touch layout", selection: $settings.controllerLayout) {
                ForEach(ControllerLayout.allCases) { Text($0.title).tag($0) }
            }
            VStack(alignment: .leading) {
                Text("Touch control opacity: \(Int(settings.touchControlOpacity * 100))%")
                Slider(value: $settings.touchControlOpacity, in: 0.15...1)
            }
            Button("Edit touch control layout") { editingLayout = true }
            Toggle("Haptic feedback", isOn: $settings.haptics)
            Toggle("Show speed (fps) while playing", isOn: $settings.showStats)
        } header: {
            Text("Input & Interface")
        } footer: {
            Text("You can also edit the layout during a game with the hand button in the toolbar. "
                 + "On modern controllers the left shoulder is White and the right shoulder is Black. "
                 + "Triggers are analog; face buttons report pressure where the controller supports it.")
        }
    }
}

/// `LabeledContent` is iOS 16+; this is the iOS 15 equivalent.
private struct LabeledRow: View {
    let label: String
    let value: String
    init(_ label: String, _ value: String) {
        self.label = label
        self.value = value
    }
    var body: some View {
        HStack {
            Text(label)
            Spacer()
            Text(value).foregroundColor(.secondary).multilineTextAlignment(.trailing)
        }
    }
}
