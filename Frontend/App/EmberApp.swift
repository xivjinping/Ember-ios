import SwiftUI

@main
struct EmberApp: App {
    @StateObject private var settings = AppSettings()
    @StateObject private var library = GameLibrary()
    @StateObject private var controllers = ControllerManager()
    @StateObject private var jit = JITCoordinator()

    var body: some Scene {
        WindowGroup {
            LibraryView()
                .environmentObject(settings)
                .environmentObject(library)
                .environmentObject(controllers)
                .environmentObject(jit)
                .preferredColorScheme(.dark)
                .task {
                    // If the app was launched through a JIT enabler, its script is
                    // attached and waiting on us right now. Complete the handshake
                    // immediately instead of at first game boot — the process-wide
                    // code cache is then ready for every session.
                    if settings.preferRecompiler, JITSupport.hasGetTaskAllow, JITSupport.isDebugged {
                        _ = await jit.acquire(settings: settings)
                    }
                }
        }
    }
}
