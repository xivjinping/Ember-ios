import SwiftUI
import UIKit

/// One game session: owns the core, audio output and input sources, and is
/// the only object the emulation screen talks to.
@MainActor
final class EmulatorSession: ObservableObject {
    let game: Game
    let core: EmulatorCore
    let input = InputMixer()

    @Published private(set) var isPaused = false
    @Published private(set) var stats: CoreStats?
    @Published private(set) var cpuNote = "Interpreter"
    @Published var audioError: String?

    /// Written by the renderer every frame; shown alongside `stats`, which
    /// republishes once a second.
    var renderInfo = ""

    private let audio = AudioEngine()
    private let controllers: ControllerManager
    private var statsTimer: Timer?
    private var started = false

    init(game: Game, settings: AppSettings, controllers: ControllerManager) throws {
        self.game = game
        self.controllers = controllers
        core = try EmulatorCore(configuration: settings.makeCoreConfiguration())
        try core.loadTitle(at: game.url)
    }

    /// Boots the guest. JIT is attempted first when requested; if it cannot be
    /// obtained the session simply runs on the interpreter.
    func start(settings: AppSettings, jit: JITCoordinator) async {
        guard !started else { return }
        started = true

        if settings.preferRecompiler {
            if await jit.acquire(settings: settings), JITSupport.attachCodeCache(to: core) {
                cpuNote = "Recompiler"
            } else {
                cpuNote = "Interpreter (JIT unavailable: \(jit.statusText))"
            }
        }

        core.start()
        do { try audio.start(core: core) } catch { audioError = error.localizedDescription }
        UIApplication.shared.isIdleTimerDisabled = true

        statsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self = self else { return }
                self.stats = self.core.stats()
            }
        }
    }

    func setPaused(_ paused: Bool) {
        guard started, paused != isPaused else { return }
        isPaused = paused
        core.setPaused(paused)
        audio.setPaused(paused)
    }

    func stop() {
        guard started else { return }
        started = false
        statsTimer?.invalidate()
        statsTimer = nil
        // Order matters: silence the audio thread before the core it reads
        // from goes away, then stop the guest.
        audio.stop()
        core.stop()
        UIApplication.shared.isIdleTimerDisabled = false
    }

    /// Called by the renderer once per display frame, so the guest never sees
    /// input older than one frame. Everything goes to controller port 1.
    func pushInput() {
        guard started, !isPaused else { return }
        core.setPad(input.snapshot(controller: controllers.poll()), port: 0)
    }
}
