import SwiftUI

/// The in-game screen: Metal view underneath, touch controls and a small
/// toolbar on top.
@MainActor
struct EmulationView: View {
    let game: Game
    @EnvironmentObject private var settings: AppSettings
    @EnvironmentObject private var controllers: ControllerManager
    @EnvironmentObject private var jit: JITCoordinator
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase

    @State private var session: EmulatorSession?
    @State private var bootError: String?
    @State private var showToolbar = true

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let session = session {
                RunningView(session: session, showToolbar: $showToolbar, onExit: quit)
            } else if let bootError = bootError {
                VStack(spacing: 16) {
                    Image(systemName: "exclamationmark.triangle").font(.largeTitle)
                    Text(bootError).multilineTextAlignment(.center)
                    Button("Close", action: { dismiss() }).buttonStyle(.borderedProminent)
                }
                .foregroundColor(.white)
                .padding()
            } else {
                ProgressView("Starting \(game.title)…").tint(.white).foregroundColor(.white)
            }
        }
        .statusBar(hidden: true)
        .persistentSystemOverlays(hiddenIfAvailable: true)
        .task { await boot() }
        .onChange(of: scenePhase) { phase in
            // iOS suspends background apps; pausing first keeps audio and the
            // guest clock consistent.
            session?.setPaused(phase != .active)
        }
        .onDisappear { session?.stop() }
    }

    private func boot() async {
        guard session == nil, bootError == nil else { return }
        do {
            let newSession = try EmulatorSession(game: game, settings: settings, controllers: controllers)
            await newSession.start(settings: settings, jit: jit)
            session = newSession
        } catch {
            bootError = error.localizedDescription
        }
    }

    private func quit() {
        session?.stop()
        dismiss()
    }
}

@MainActor
private struct RunningView: View {
    @ObservedObject var session: EmulatorSession
    @Binding var showToolbar: Bool
    let onExit: () -> Void
    @EnvironmentObject private var settings: AppSettings
    @EnvironmentObject private var controllers: ControllerManager

    var body: some View {
        ZStack(alignment: .top) {
            MetalRenderView(session: session, renderScale: settings.renderScale, fxaa: settings.fxaa,
                            upscaler: settings.upscaler, sharpness: settings.sharpness, aspect: settings.aspect,
                            allow120Hz: settings.allow120Hz)
                .ignoresSafeArea()

            // A physical controller replaces the touch controls.
            if !controllers.isConnected {
                VirtualGamePadView(input: session.input, layout: settings.controllerLayout,
                                   opacity: settings.touchControlOpacity, haptics: settings.haptics)
            }

            VStack(spacing: 6) {
                toolbar
                if EmulatorCore.backendName == "stub" {
                    VStack(spacing: 2) {
                        Text("Stub core: test pattern only. No engine is linked.")
                        if let status = session.stats?.status, !status.isEmpty { Text(status) }
                    }
                    .font(.caption2).multilineTextAlignment(.center).padding(6)
                    .background(Color.orange.opacity(0.85)).cornerRadius(6)
                    .padding(.horizontal, 60)
                } else if let status = session.stats?.status, !status.isEmpty {
                    // What the console is doing, or what it still needs.
                    Text(status)
                        .font(.caption2).multilineTextAlignment(.center).padding(6)
                        .background(Color.black.opacity(0.6)).cornerRadius(6)
                        .padding(.horizontal, 60)
                }
                if settings.showStats, let stats = session.stats {
                    Text(String(format: "%.0f fps · %@ · %ld MB RAM\n%@", stats.fps, session.cpuNote, Int(stats.ramMB),
                                session.renderInfo))
                        .font(.caption2.monospacedDigit()).multilineTextAlignment(.center).padding(6)
                        .background(Color.black.opacity(0.55)).cornerRadius(6)
                }
            }
            .foregroundColor(.white)
            .padding(.top, 6)
        }
    }

    private var toolbar: some View {
        HStack(spacing: 14) {
            if showToolbar {
                Button(action: onExit) { Image(systemName: "xmark") }
                    .accessibilityLabel("Quit game")
                Button(action: { session.setPaused(!session.isPaused) }) {
                    Image(systemName: session.isPaused ? "play.fill" : "pause.fill")
                }
                .accessibilityLabel(session.isPaused ? "Resume" : "Pause")

                // Live graphics switches: each takes effect on the next frame.
                Menu {
                    Picker("Resolution", selection: $settings.renderScale) {
                        ForEach(RenderScale.allCases) { Text($0.title).tag($0) }
                    }
                    Picker("Upscaler", selection: $settings.upscaler) {
                        ForEach(Upscaler.allCases) { Text($0.shortTitle).tag($0) }
                    }
                    Toggle("FXAA", isOn: $settings.fxaa)
                    Picker("Aspect", selection: $settings.aspect) {
                        ForEach(AspectMode.allCases) { Text($0.title).tag($0) }
                    }
                    Toggle("Show statistics", isOn: $settings.showStats)
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "sparkles.tv")
                        Text(settings.renderScale.shortTitle).font(.caption.weight(.bold))
                    }
                }
                .accessibilityLabel("Graphics")
            }
            Button(action: { withAnimation { showToolbar.toggle() } }) {
                Image(systemName: showToolbar ? "chevron.up" : "chevron.down")
            }
            .accessibilityLabel(showToolbar ? "Hide toolbar" : "Show toolbar")
        }
        .font(.body.weight(.semibold))
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(Color.black.opacity(0.55))
        .clipShape(Capsule())
    }
}

private extension View {
    /// Hides the home indicator while playing (iOS 16+; no-op on iOS 15).
    @ViewBuilder
    func persistentSystemOverlays(hiddenIfAvailable hidden: Bool) -> some View {
        if #available(iOS 16.0, *) {
            persistentSystemOverlays(hidden ? .hidden : .automatic)
        } else {
            self
        }
    }
}
