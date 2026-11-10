import CoreGraphics
import GameController

/// Physical controllers through the GameController framework: MFi, DualSense /
/// DualShock 4, Xbox Wireless, Switch Pro — anything with the extended-gamepad
/// profile. Polled once per frame rather than through value handlers, so all
/// inputs in a frame are sampled at the same instant.
@MainActor
final class ControllerManager: ObservableObject {
    @Published private(set) var controllerName: String?
    private var controller: GCController?
    private var observers: [NSObjectProtocol] = []

    var isConnected: Bool { controller != nil }

    init() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
            let connected = note.object as? GCController
            Task { @MainActor in self?.adopt(connected) }
        })
        observers.append(center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] note in
            let gone = note.object as? GCController
            Task { @MainActor in
                guard let self = self, gone === self.controller else { return }
                self.adopt(GCController.controllers().first { $0.extendedGamepad != nil && $0 !== gone })
            }
        })
        adopt(GCController.current ?? GCController.controllers().first { $0.extendedGamepad != nil })
        GCController.startWirelessControllerDiscovery(completionHandler: nil)
    }

    deinit {
        observers.forEach(NotificationCenter.default.removeObserver)
    }

    private func adopt(_ candidate: GCController?) {
        guard candidate?.extendedGamepad != nil || candidate == nil else { return }
        controller = candidate
        controllerName = candidate?.vendorName ?? candidate?.productCategory
    }

    /// Current state of the active controller, or nil if none is connected.
    ///
    /// GameController already names buttons in the Xbox arrangement, so the
    /// face buttons map one to one on every pad (a DualSense's Cross is A).
    /// Modern pads have no Black and White; they take the shoulder buttons,
    /// the convention later Xbox consoles used for original-Xbox games:
    /// left shoulder = White, right shoulder = Black.
    func poll() -> PadInput? {
        guard let pad = controller?.extendedGamepad else { return nil }
        var input = PadInput()

        // Face buttons report pressure where the hardware has it (value 0...1)
        // and plain on/off otherwise; both end up as 0...255.
        func pressure(_ button: GCControllerButtonInput) -> UInt8 {
            let value = max(button.value, button.isPressed ? 1 : 0)
            return UInt8((min(max(value, 0), 1) * 255).rounded())
        }
        input[.a] = pressure(pad.buttonA)
        input[.b] = pressure(pad.buttonB)
        input[.x] = pressure(pad.buttonX)
        input[.y] = pressure(pad.buttonY)
        input[.white] = pressure(pad.leftShoulder)
        input[.black] = pressure(pad.rightShoulder)
        input[.leftTrigger] = UInt8((min(max(pad.leftTrigger.value, 0), 1) * 255).rounded())
        input[.rightTrigger] = UInt8((min(max(pad.rightTrigger.value, 0), 1) * 255).rounded())

        func set(_ button: PadButtons, _ pressed: Bool?) {
            if pressed == true { input.buttons.insert(button) }
        }
        set(.dpadUp, pad.dpad.up.isPressed)
        set(.dpadDown, pad.dpad.down.isPressed)
        set(.dpadLeft, pad.dpad.left.isPressed)
        set(.dpadRight, pad.dpad.right.isPressed)
        set(.start, pad.buttonMenu.isPressed)
        set(.back, pad.buttonOptions?.isPressed)
        set(.leftThumb, pad.leftThumbstickButton?.isPressed)
        set(.rightThumb, pad.rightThumbstickButton?.isPressed)

        input.leftStick = CGPoint(x: CGFloat(pad.leftThumbstick.xAxis.value), y: CGFloat(pad.leftThumbstick.yAxis.value))
        input.rightStick = CGPoint(x: CGFloat(pad.rightThumbstick.xAxis.value), y: CGFloat(pad.rightThumbstick.yAxis.value))
        return input
    }
}
