import SwiftUI
import UIKit

/// On-screen controller in the original pad's arrangement: left stick above
/// the D-pad, face buttons above the right stick, Black and White beside the
/// face buttons, triggers in the top corners, Back and Start in the middle.
/// Only the controls intercept touches; the picture stays visible between them.
@MainActor
struct VirtualGamePadView: View {
    let input: InputMixer
    var layout: ControllerLayout
    var opacity: Double
    var haptics: Bool
    /// While true the controls send no input and can be dragged and resized.
    var editing: Bool = false
    @ObservedObject private var store = PadLayoutStore.shared

    // The pad's own face-button colours.
    private static let green = Color(red: 0.30, green: 0.78, blue: 0.35)
    private static let red = Color(red: 0.90, green: 0.25, blue: 0.22)
    private static let blue = Color(red: 0.22, green: 0.48, blue: 0.92)
    private static let yellow = Color(red: 0.95, green: 0.80, blue: 0.20)

    var body: some View {
        GeometryReader { proxy in
            let compact = proxy.size.height < 420
            let unit: CGFloat = compact ? 46 : 58

            ZStack {
                // Triggers
                VStack {
                    HStack {
                        analogKey("LT", .leftTrigger, triggerSize("LT", unit))
                            .padEditable(.leftTrigger, editing: editing, store: store)
                        Spacer()
                        analogKey("RT", .rightTrigger, triggerSize("RT", unit))
                            .padEditable(.rightTrigger, editing: editing, store: store)
                    }
                    Spacer()
                }

                // Left: stick (press = L3 key beside it) above D-pad
                VStack(alignment: .leading, spacing: 12) {
                    Spacer()
                    HStack(alignment: .bottom, spacing: 8) {
                        StickView(diameter: unit * 2.1, knob: ControllerIcons.image(for: "LSTICK")) { input.virtualPad.leftStick = $0 }
                            .padEditable(.leftStick, editing: editing, store: store)
                        digitalKey("L3", .leftThumb, unit * 0.62)
                            .padEditable(.leftThumb, editing: editing, store: store)
                    }
                    dpad(unit)
                        .padEditable(.dpad, editing: editing, store: store)
                }
                .frame(maxWidth: .infinity, alignment: .leading)

                // Right: face buttons (with Black/White) above stick
                VStack(alignment: .trailing, spacing: 12) {
                    Spacer()
                    faceCluster(unit)
                    HStack(alignment: .bottom, spacing: 8) {
                        digitalKey("R3", .rightThumb, unit * 0.62)
                            .padEditable(.rightThumb, editing: editing, store: store)
                        StickView(diameter: unit * 1.7, knob: ControllerIcons.image(for: "RSTICK")) { input.virtualPad.rightStick = $0 }
                            .padEditable(.rightStick, editing: editing, store: store)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .trailing)

                // Back / Start
                VStack {
                    Spacer()
                    HStack(spacing: 22) {
                        digitalKey("BACK", .back, unit * 0.62, wide: true)
                            .padEditable(.back, editing: editing, store: store)
                        digitalKey("START", .start, unit * 0.62, wide: true)
                            .padEditable(.start, editing: editing, store: store)
                    }
                }
            }
            .padding(.horizontal, 18)
            .padding(.vertical, 10)
        }
        .opacity(opacity)
    }

    /// A wide text key, or the shape of the picture when there is one.
    private func triggerSize(_ label: String, _ unit: CGFloat) -> CGSize {
        if let image = ControllerIcons.image(for: label), image.size.height > 0 {
            let height = unit * 1.15
            return CGSize(width: height * image.size.width / image.size.height, height: height)
        }
        return CGSize(width: unit * 1.6, height: unit * 0.75)
    }

    private func faceCluster(_ unit: CGFloat) -> some View {
        let small = CGSize(width: unit * 0.62, height: unit * 0.62)
        let blackWhite = HStack(spacing: 10) {
            analogKey("WHT", .white, small, tint: .white)
            analogKey("BLK", .black, small, tint: Color(white: 0.35))
        }
        let diamond = VStack(spacing: 2) {
            analogKey("Y", .y, CGSize(width: unit, height: unit), tint: Self.yellow)
            HStack(spacing: unit * 0.9) {
                analogKey("X", .x, CGSize(width: unit, height: unit), tint: Self.blue)
                analogKey("B", .b, CGSize(width: unit, height: unit), tint: Self.red)
            }
            analogKey("A", .a, CGSize(width: unit, height: unit), tint: Self.green)
        }
        return VStack(alignment: .trailing, spacing: 8) {
            if layout == .duke { blackWhite.padEditable(.blackWhite, editing: editing, store: store) }
            diamond.padEditable(.faceButtons, editing: editing, store: store)
            if layout == .controllerS { blackWhite.padEditable(.blackWhite, editing: editing, store: store) }
        }
    }

    private func dpad(_ unit: CGFloat) -> some View {
        let s = unit * 0.82
        return VStack(spacing: 2) {
            digitalKey("▲", .dpadUp, s)
            HStack(spacing: s) { digitalKey("◀", .dpadLeft, s); digitalKey("▶", .dpadRight, s) }
            digitalKey("▼", .dpadDown, s)
        }
    }

    private func digitalKey(_ label: String, _ button: PadButtons, _ size: CGFloat, wide: Bool = false) -> some View {
        PadKey(label: label, size: CGSize(width: wide ? size * 2.1 : size, height: size), tint: .white, haptics: haptics) { pressed in
            if pressed { input.virtualPad.buttons.insert(button) } else { input.virtualPad.buttons.remove(button) }
        }
    }

    /// A touch screen cannot sense pressure, so these report fully pressed (255).
    private func analogKey(_ label: String, _ button: AnalogButton, _ size: CGSize, tint: Color = .white) -> some View {
        PadKey(label: label, size: size, tint: tint, haptics: haptics) { pressed in
            input.virtualPad[button] = pressed ? 255 : 0
        }
    }
}

/// Custom button pictures: PNGs in Resources/ControllerIcons, named after
/// the button (A.png, LT.png, BACK.png, UP.png ...). A missing file keeps
/// the drawn text label.
enum ControllerIcons {
    private static var cache: [String: UIImage?] = [:]

    static func fileName(for label: String) -> String {
        switch label {
        case "▲": return "UP"
        case "▼": return "DOWN"
        case "◀": return "LEFT"
        case "▶": return "RIGHT"
        default: return label
        }
    }

    static func image(for label: String) -> UIImage? {
        let name = fileName(for: label)
        if let cached = cache[name] { return cached }
        let image = Bundle.main.path(forResource: name, ofType: "png", inDirectory: "ControllerIcons")
            .flatMap { UIImage(contentsOfFile: $0) }
        cache[name] = image
        return image
    }
}

/// A button that reports press and release. A zero-distance drag gesture is
/// used instead of Button so the press registers on touch-down, not touch-up.
private struct PadKey: View {
    let label: String
    let size: CGSize
    let tint: Color
    let haptics: Bool
    let onChange: (Bool) -> Void
    @State private var pressed = false

    var body: some View {
        let radius = min(size.width, size.height) / 2
        let image = ControllerIcons.image(for: label)
        face
            .frame(width: size.width, height: size.height)
            .scaleEffect(image != nil && pressed ? 0.9 : 1)
            .brightness(image != nil && pressed ? -0.25 : 0)
            .background(Group {
                if image == nil {
                    RoundedRectangle(cornerRadius: radius, style: .continuous)
                        .fill(tint.opacity(pressed ? 0.60 : 0.22))
                        .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous).stroke(tint.opacity(0.75), lineWidth: 1.5))
                }
            })
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        guard !pressed else { return }
                        pressed = true
                        onChange(true)
                        if haptics { UIImpactFeedbackGenerator(style: .light).impactOccurred() }
                    }
                    .onEnded { _ in
                        pressed = false
                        onChange(false)
                    }
            )
            .accessibilityLabel(Text(label))
            .accessibilityAddTraits(.isButton)
    }

    @ViewBuilder
    private var face: some View {
        if let image = ControllerIcons.image(for: label) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
        } else {
            Text(label)
                .font(.system(size: min(size.width, size.height) * (label.count > 2 ? 0.30 : 0.40), weight: .bold, design: .rounded))
                .foregroundColor(.white)
        }
    }
}

/// Analog stick: drag anywhere inside the ring; output is -1...1 with up = +y.
private struct StickView: View {
    let diameter: CGFloat
    var knob: UIImage? = nil   // RSTICK.png / LSTICK.png, if provided
    let onChange: (CGPoint) -> Void
    @State private var offset = CGSize.zero

    var body: some View {
        let radius = diameter / 2
        let travel = radius * 0.6
        ZStack {
            Circle().fill(Color.white.opacity(0.10))
            Circle().stroke(Color.white.opacity(0.45), lineWidth: 1.5)
            Group {
                if let knob = knob {
                    Image(uiImage: knob).resizable().scaledToFit()
                } else {
                    Circle().fill(Color.white.opacity(0.40))
                }
            }
            .frame(width: radius * 0.9, height: radius * 0.9)
            .offset(offset)
        }
        .frame(width: diameter, height: diameter)
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    var dx = value.location.x - radius
                    var dy = value.location.y - radius
                    let length = max(sqrt(dx * dx + dy * dy), 0.0001)
                    if length > travel {
                        dx *= travel / length
                        dy *= travel / length
                    }
                    offset = CGSize(width: dx, height: dy)
                    onChange(CGPoint(x: dx / travel, y: -dy / travel))
                }
                .onEnded { _ in
                    offset = .zero
                    onChange(.zero)
                }
        )
        .accessibilityHidden(true)
    }
}
