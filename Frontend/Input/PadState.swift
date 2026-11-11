import CoreGraphics
import Foundation
import QuartzCore

/// Digital buttons. Raw values are the guest's XINPUT_GAMEPAD bits and must
/// stay equal to the EM_PAD_* constants in Core/include/ember/core_api.h.
struct PadButtons: OptionSet, Hashable {
    let rawValue: UInt16
    static let dpadUp     = PadButtons(rawValue: 0x0001)
    static let dpadDown   = PadButtons(rawValue: 0x0002)
    static let dpadLeft   = PadButtons(rawValue: 0x0004)
    static let dpadRight  = PadButtons(rawValue: 0x0008)
    static let start      = PadButtons(rawValue: 0x0010)
    static let back       = PadButtons(rawValue: 0x0020)
    static let leftThumb  = PadButtons(rawValue: 0x0040)
    static let rightThumb = PadButtons(rawValue: 0x0080)
}

/// The pressure-sensitive controls, in the order of the EM_ANALOG_* indices.
enum AnalogButton: Int, CaseIterable {
    case a, b, x, y, black, white, leftTrigger, rightTrigger
}

/// One input source's contribution (touch overlay, or a physical controller).
struct PadInput {
    var buttons: PadButtons = []
    var analog = [UInt8](repeating: 0, count: AnalogButton.allCases.count) // 0...255 each
    var leftStick = CGPoint.zero  // -1...1, up is +y
    var rightStick = CGPoint.zero

    subscript(button: AnalogButton) -> UInt8 {
        get { analog[button.rawValue] }
        set { analog[button.rawValue] = newValue }
    }
}

/// Merges every input source into the single controller state the core sees
/// on port 1. Main-thread only: written by SwiftUI gestures and read once per
/// display frame by the renderer.
@MainActor
final class InputMixer {
    /// On-screen controls. A tap can begin and end between two display frames
    /// (the main thread is busy), which would make the press invisible to the
    /// guest. Every new press is therefore latched for `minimumHold` seconds.
    var virtualPad = PadInput() {
        didSet { latchNewPresses(from: oldValue, to: virtualPad) }
    }

    private static let minimumHold: CFTimeInterval = 0.07
    private var buttonHold: [UInt16: CFTimeInterval] = [:]  // digital bit -> hold until
    private var analogHold: [Int: CFTimeInterval] = [:]     // analog index -> hold until

    private func latchNewPresses(from old: PadInput, to new: PadInput) {
        let until = CACurrentMediaTime() + Self.minimumHold
        let added = new.buttons.subtracting(old.buttons).rawValue
        for bit in 0..<16 where added & (1 << bit) != 0 { buttonHold[1 << bit] = until }
        for i in new.analog.indices where old.analog[i] == 0 && new.analog[i] > 0 { analogHold[i] = until }
    }

    /// The touch pad as the guest should see it right now: held controls plus
    /// presses that were released too quickly to be sampled.
    private func effectiveVirtualPad() -> PadInput {
        let now = CACurrentMediaTime()
        var pad = virtualPad
        for (bit, until) in buttonHold {
            if now < until { pad.buttons.insert(PadButtons(rawValue: bit)) } else { buttonHold[bit] = nil }
        }
        for (i, until) in analogHold {
            if now < until { pad.analog[i] = 255 } else { analogHold[i] = nil }
        }
        return pad
    }

    func snapshot(controller: PadInput?) -> em_pad_state {
        let physical = controller ?? PadInput()
        let virtualPad = effectiveVirtualPad()
        var state = em_pad_state()
        state.buttons = virtualPad.buttons.union(physical.buttons).rawValue

        // Whichever source presses harder wins. Touch buttons are all-or-nothing (255).
        let merged = zip(virtualPad.analog, physical.analog).map { max($0, $1) }
        state.analog = (merged[0], merged[1], merged[2], merged[3], merged[4], merged[5], merged[6], merged[7])

        let left = Self.dominant(virtualPad.leftStick, physical.leftStick)
        let right = Self.dominant(virtualPad.rightStick, physical.rightStick)
        state.lx = Self.axis(left.x);  state.ly = Self.axis(left.y)
        state.rx = Self.axis(right.x); state.ry = Self.axis(right.y)
        return state
    }

    /// The stick that is pushed further wins, so a thumb resting on the touch
    /// stick does not fight a physical controller.
    private static func dominant(_ a: CGPoint, _ b: CGPoint) -> CGPoint {
        (a.x * a.x + a.y * a.y) >= (b.x * b.x + b.y * b.y) ? a : b
    }

    private static func axis(_ value: CGFloat) -> Int16 {
        Int16(max(-32768, min(32767, (value * 32767).rounded())))
    }
}
