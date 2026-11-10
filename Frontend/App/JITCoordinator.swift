import SwiftUI
import UIKit

/// Drives the JIT acquisition sequence described in docs/JIT.md.
///
///   1. Refuse early if the install has no `get-task-allow`.
///   2. If a debugger has not attached yet, ask the chosen enabler to attach
///      (StikDebug URL) or simply wait for one (AltJIT, SideJITServer, Xcode).
///   3. Poll CS_DEBUGGED. Nothing JIT-related runs before it is set.
///   4. Allocate the dual-mapped code cache once for the whole process —
///      through the TXM breakpoint protocol when the device has TXM. Each
///      game session then attaches to it (JITSupport.attachCodeCache(to:)).
///
/// Any failure leaves the session on the interpreter; it never blocks boot.
@MainActor
final class JITCoordinator: ObservableObject {
    enum State: Equatable {
        case idle
        case unavailable(String)   // this install can never have JIT
        case waitingForDebugger
        case preparing
        case ready
        case failed(String)
    }

    @Published private(set) var state: State = .idle

    /// TXM-protocol script understood by the allocator in jit_memory.cpp.
    /// Developer-defined: never a user setting.
    private static let scriptName = "universal.js"

    var statusText: String {
        switch state {
        case .idle: return JITSupport.isDebugged ? "Debugger attached" : "Not enabled"
        case .unavailable(let why): return why
        case .waitingForDebugger: return "Waiting for debugger…"
        case .preparing: return "Preparing code cache…"
        case .ready: return "Enabled"
        case .failed(let why): return why
        }
    }

    func usesTXMProtocol(_ settings: AppSettings) -> Bool {
        settings.forceJITScript || JITSupport.txmStatus == .present
    }

    /// Returns true once the process-wide code cache exists. Safe to call
    /// repeatedly: at launch, from Settings, and before each game boot.
    func acquire(settings: AppSettings, timeout: TimeInterval = 45) async -> Bool {
        if JITSupport.isCodeCacheReady {
            state = .ready
            return true
        }
        guard JITSupport.hasGetTaskAllow else {
            state = .unavailable("This install lacks get-task-allow. Re-sign with a development profile.")
            return false
        }

        let txm = usesTXMProtocol(settings)
        if JITSupport.txmStatus == .unknown && !settings.forceJITScript {
            // Guessing wrong either crashes (brk with nobody listening) or yields
            // pages that die on first execution, so make the user decide.
            state = .failed("Could not tell whether this device has TXM. Set “Force JIT script” in Settings.")
            return false
        }

        if !JITSupport.isDebugged {
            state = .waitingForDebugger
            if settings.jitMethod == .stikDebug { requestStikDebug(includeScript: txm) }
            let deadline = Date().addingTimeInterval(timeout)
            while !JITSupport.isDebugged {
                if Date() > deadline || Task.isCancelled {
                    state = .failed("No debugger attached within \(Int(timeout)) s.")
                    return false
                }
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
        }
        // If the process was already debugged on entry (the usual "launch with
        // JIT" flow), the enabler's script is attached and waiting for us.

        state = .preparing
        let bytes = UInt(settings.codeCacheMB * 1_048_576)
        let failure: String? = await Task.detached(priority: .userInitiated) {
            do {
                try JITSupport.prepareCodeCache(bytes: bytes, useTXMProtocol: txm)
                return nil
            } catch {
                return error.localizedDescription
            }
        }.value

        if let failure = failure {
            state = .failed(failure)
            return false
        }
        state = .ready
        return true
    }

    private func requestStikDebug(includeScript: Bool) {
        guard let bundleID = Bundle.main.bundleIdentifier else { return }
        var components = URLComponents()
        components.scheme = "stikdebug"
        components.host = "enable-jit"
        components.queryItems = [
            URLQueryItem(name: "bundle-id", value: bundleID),
            URLQueryItem(name: "pid", value: String(getpid())),
        ]
        if includeScript {
            components.queryItems?.append(URLQueryItem(name: "script-name", value: Self.scriptName))
        }
        guard let url = components.url else { return }
        // Success here only means iOS delivered the URL. Readiness is decided
        // by CS_DEBUGGED and the allocator, never by this call.
        UIApplication.shared.open(url)
    }
}
