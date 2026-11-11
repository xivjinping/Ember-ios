import SwiftUI

/// The movable parts of the touch controller.
enum PadControl: String, CaseIterable, Codable {
    case leftTrigger, rightTrigger
    case leftStick, leftThumb
    case dpadUp, dpadDown, dpadLeft, dpadRight
    case a, b, x, y, black, white
    case rightStick, rightThumb
    case back, start

    var title: String {
        switch self {
        case .leftTrigger: return "Left trigger"
        case .rightTrigger: return "Right trigger"
        case .leftStick: return "Left stick"
        case .leftThumb: return "L3"
        case .dpadUp: return "D-pad up"
        case .dpadDown: return "D-pad down"
        case .dpadLeft: return "D-pad left"
        case .dpadRight: return "D-pad right"
        case .a: return "A"
        case .b: return "B"
        case .x: return "X"
        case .y: return "Y"
        case .black: return "Black"
        case .white: return "White"
        case .rightStick: return "Right stick"
        case .rightThumb: return "R3"
        case .back: return "Back"
        case .start: return "Start"
        }
    }
}

/// Where the user moved one control and how big they made it, relative to
/// its default spot.
struct PadItemLayout: Codable, Equatable {
    var dx: Double = 0
    var dy: Double = 0
    var scale: Double = 1
}

/// The user's touch-controller layout, saved across launches.
@MainActor
final class PadLayoutStore: ObservableObject {
    static let shared = PadLayoutStore()
    static let scaleRange: ClosedRange<Double> = 0.5...2.0

    @Published private(set) var items: [PadControl: PadItemLayout] = [:]
    /// Size of every control together, on top of each one's own size.
    @Published var globalScale: Double = 1 { didSet { save() } }
    /// The control whose size slider is shown in the editor.
    @Published var selected: PadControl?

    private let defaults: UserDefaults
    private static let key = "input.padLayout"

    private struct Saved: Codable {
        var items: [String: PadItemLayout]
        var globalScale: Double
    }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let data = defaults.data(forKey: Self.key),
           let saved = try? JSONDecoder().decode(Saved.self, from: data) {
            var loaded: [PadControl: PadItemLayout] = [:]
            for (name, item) in saved.items {
                if let control = PadControl(rawValue: name) { loaded[control] = item }
            }
            items = loaded
            globalScale = saved.globalScale
        }
    }

    func item(_ control: PadControl) -> PadItemLayout { items[control] ?? PadItemLayout() }

    func move(_ control: PadControl, by translation: CGSize) {
        var item = item(control)
        item.dx += Double(translation.width)
        item.dy += Double(translation.height)
        items[control] = item
        save()
    }

    func setScale(_ control: PadControl, _ scale: Double) {
        var item = item(control)
        item.scale = min(max(scale, Self.scaleRange.lowerBound), Self.scaleRange.upperBound)
        items[control] = item
        save()
    }

    func reset() {
        items = [:]
        selected = nil
        globalScale = 1 // saves
    }

    private func save() {
        var named: [String: PadItemLayout] = [:]
        for (control, item) in items { named[control.rawValue] = item }
        if let data = try? JSONEncoder().encode(Saved(items: named, globalScale: globalScale)) {
            defaults.set(data, forKey: Self.key)
        }
    }
}

/// Applies the saved position and size to one control and, while editing,
/// turns it into a draggable, selectable block that sends no input.
private struct PadEditable: ViewModifier {
    let control: PadControl
    let editing: Bool
    @ObservedObject var store: PadLayoutStore
    @State private var drag = CGSize.zero

    func body(content: Content) -> some View {
        let item = store.item(control)
        let isSelected = store.selected == control
        content
            .allowsHitTesting(!editing)
            .overlay(Group { if editing { editFrame(isSelected) } })
            .scaleEffect(CGFloat(item.scale * store.globalScale))
            .offset(x: CGFloat(item.dx) + drag.width, y: CGFloat(item.dy) + drag.height)
    }

    private func editFrame(_ isSelected: Bool) -> some View {
        RoundedRectangle(cornerRadius: 10, style: .continuous)
            .stroke(isSelected ? Color.yellow : Color.white.opacity(0.7),
                    style: StrokeStyle(lineWidth: isSelected ? 3 : 1.5, dash: [6, 4]))
            .background(Color.white.opacity(isSelected ? 0.15 : 0.05))
            .padding(-4)
            .contentShape(Rectangle())
            // Global space, so the finger and the control move together
            // whatever the control's size.
            .gesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .global)
                    .onChanged { value in
                        if store.selected != control { store.selected = control }
                        drag = value.translation
                    }
                    .onEnded { value in
                        store.move(control, by: value.translation)
                        drag = .zero
                    }
            )
    }
}

extension View {
    func padEditable(_ control: PadControl, editing: Bool, store: PadLayoutStore) -> some View {
        modifier(PadEditable(control: control, editing: editing, store: store))
    }
}

/// The bar shown while the layout is edited: size of the selected control
/// (or of all of them), reset, done.
@MainActor
struct PadLayoutEditBar: View {
    @ObservedObject var store: PadLayoutStore
    let onDone: () -> Void

    var body: some View {
        VStack(spacing: 8) {
            Text(store.selected.map { "\($0.title): drag to move, slider to resize" }
                 ?? "Drag any control to move it. Tap one to resize it.")
                .font(.caption.weight(.semibold))
            HStack(spacing: 12) {
                Image(systemName: "minus.magnifyingglass")
                if let control = store.selected {
                    Slider(value: Binding(get: { store.item(control).scale },
                                          set: { store.setScale(control, $0) }),
                           in: PadLayoutStore.scaleRange)
                } else {
                    Slider(value: $store.globalScale, in: PadLayoutStore.scaleRange)
                }
                Image(systemName: "plus.magnifyingglass")
            }
            HStack(spacing: 16) {
                if store.selected != nil {
                    Button("All controls") { store.selected = nil }
                }
                Button("Reset", role: .destructive) { store.reset() }
                Button("Done", action: onDone).buttonStyle(.borderedProminent)
            }
            .font(.callout.weight(.semibold))
        }
        .foregroundColor(.white)
        .padding(12)
        .frame(maxWidth: 420)
        .background(Color.black.opacity(0.75))
        .cornerRadius(14)
    }
}

/// Full-screen layout editor reached from Settings, over a plain backdrop.
@MainActor
struct PadLayoutEditorScreen: View {
    @EnvironmentObject private var settings: AppSettings
    @Environment(\.dismiss) private var dismiss
    @StateObject private var preview = PreviewInput()

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(white: 0.18), Color(white: 0.06)], startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()
            VirtualGamePadView(input: preview.mixer, layout: settings.controllerLayout,
                               opacity: max(settings.touchControlOpacity, 0.6), haptics: false, editing: true)
            PadLayoutEditBar(store: PadLayoutStore.shared) { dismiss() }
                .padding(.top, 40)
                .frame(maxHeight: .infinity, alignment: .top)
        }
        .statusBar(hidden: true)
    }

    /// The editor sends no input anywhere; the pad view just needs a mixer.
    @MainActor
    final class PreviewInput: ObservableObject {
        let mixer = InputMixer()
    }
}
