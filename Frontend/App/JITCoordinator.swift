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

    /// nil when the setting is Automatic and detection could not tell.
    func usesTXMProtocol(_ settings: AppSettings) -> Bool? {
        switch settings.jitProtocol {
        case .script: return true
        case .plain: return false
        case .automatic:
            switch JITSupport.txmStatus {
            case .present: return true
            case .absent: return false
            default: return nil
            }
        }
    }

    private static func describe(_ status: TXMStatus) -> String {
        switch status {
        case .present: return "present"
        case .absent: return "absent"
        default: return "unknown"
        }
    }

    /// Returns true once the process-wide code cache exists. Safe to call
    /// repeatedly: at launch, from Settings, and before each game boot.
    func acquire(settings: AppSettings, timeout: TimeInterval = 45) async -> Bool {
        if JITSupport.isCodeCacheReady {
            state = .ready
            return true
        }
        // Everything from here on is written to the trace file first, so a
        // crash leaves a record of how far it got (Settings > JIT log).
        let version = UIDevice.current.systemVersion
        JITSupport.trace("=== JIT attempt: \(JITSupport.deviceModel), iOS \(version), "
                         + "bundle \(Bundle.main.bundleIdentifier ?? "?"), pid \(getpid())")
        JITSupport.trace("get-task-allow \(JITSupport.hasGetTaskAllow ? "yes" : "NO"), "
                         + "TXM detected: \(Self.describe(JITSupport.txmStatus)), protocol setting: \(settings.jitProtocol.rawValue), "
                         + "debugger allocates: \(settings.jitDebuggerAllocates), method: \(settings.jitMethod.rawValue), "
                         + "already debugged: \(JITSupport.isDebugged)")

        guard JITSupport.hasGetTaskAllow else {
            state = .unavailable("This install lacks get-task-allow. Re-sign with a development profile.")
            JITSupport.trace("stopping: no get-task-allow")
            return false
        }

        guard let txm = usesTXMProtocol(settings) else {
            // Guessing wrong either crashes (brk with nobody listening) or yields
            // pages that die on first execution, so make the user decide.
            state = .failed("Could not tell whether this device has TXM. Choose a JIT protocol in Settings.")
            JITSupport.trace("stopping: TXM unknown and protocol is Automatic")
            return false
        }

        if !JITSupport.isDebugged {
            state = .waitingForDebugger
            if settings.jitMethod == .stikDebug { requestStikDebug(includeScript: txm) }
            let deadline = Date().addingTimeInterval(timeout)
            while !JITSupport.isDebugged {
                if Date() > deadline || Task.isCancelled {
                    state = .failed("No debugger attached within \(Int(timeout)) s.")
                    JITSupport.trace("stopping: no debugger attached within \(Int(timeout)) s")
                    return false
                }
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
            JITSupport.trace("debugger attached (CS_DEBUGGED is set)")
        }
        // If the process was already debugged on entry (the usual "launch with
        // JIT" flow), the enabler's script is attached and waiting for us.

        state = .preparing
        let bytes = UInt(settings.codeCacheMB * 1_048_576)
        let debuggerAllocates = settings.jitDebuggerAllocates
        let failure: String? = await Task.detached(priority: .userInitiated) {
            do {
                try JITSupport.prepareCodeCache(bytes: bytes, useTXMProtocol: txm, debuggerAllocates: debuggerAllocates)
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
        JITSupport.trace("opening \(url.absoluteString)")
        // Success here only means iOS delivered the URL. Readiness is decided
        // by CS_DEBUGGED and the allocator, never by this call.
        UIApplication.shared.open(url) { delivered in
            JITSupport.trace(delivered ? "StikDebug link delivered" : "StikDebug link NOT delivered (is StikDebug installed?)")
        }
    }
}
