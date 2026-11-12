import SwiftUI
import UniformTypeIdentifiers

@MainActor
struct LibraryView: View {
    @EnvironmentObject private var library: GameLibrary
    @EnvironmentObject private var settings: AppSettings
    @EnvironmentObject private var jit: JITCoordinator
    @State private var jitBusy = false
    @State private var jitMessage: String?
    @State private var running: Game?
    @State private var showImporter = false
    @State private var showSettings = false
    @State private var query = ""

    private let columns = [GridItem(.adaptive(minimum: 150, maximum: 220), spacing: 16)]

    private var visible: [Game] {
        guard !query.isEmpty else { return library.games }
        return library.games.filter {
            $0.title.localizedCaseInsensitiveContains(query) || $0.idLabel.localizedCaseInsensitiveContains(query)
        }
    }

    var body: some View {
        NavigationView {
            Group {
                if library.games.isEmpty {
                    emptyState
                } else {
                    ScrollView {
                        LazyVGrid(columns: columns, spacing: 16) {
                            ForEach(visible) { game in
                                Button(action: { running = game }) { GameCard(game: game) }
                                    .buttonStyle(.plain)
                            }
                        }
                        .padding()
                    }
                    .searchable(text: $query, prompt: "Search by title or ID")
                }
            }
            .navigationTitle("Library")
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button(action: { showSettings = true }) { Image(systemName: "gearshape") }
                        .accessibilityLabel("Settings")
                }
                ToolbarItemGroup(placement: .navigationBarTrailing) {
                    if library.isScanning { ProgressView() }
                    Button(action: { Task { await library.rescan() } }) { Image(systemName: "arrow.clockwise") }
                        .accessibilityLabel("Rescan")
                    Button(action: { showImporter = true }) { Image(systemName: "plus") }
                        .accessibilityLabel("Add games")
#if !EMBER_NOJIT
                    jitButton
#endif
                }
            }
        }
        .navigationViewStyle(.stack)
        .alert("JIT was not enabled", isPresented: Binding(get: { jitMessage != nil }, set: { if !$0 { jitMessage = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(jitMessage ?? "")
        }
        .task { await library.rescan() }
        // Files and folders are opened in place, not copied (see GameLibrary).
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.item, .folder],
                      allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { library.addPicked(urls) }
        }
        .sheet(isPresented: $showSettings) { SettingsView() }
        .fullScreenCover(item: $running) { game in EmulationView(game: game) }
    }

    /// Top-right shortcut for the step that has to be repeated every time the
    /// app is opened: hand off to StikDebug and set up JIT memory.
    private var jitButton: some View {
        let ready = jit.state == .ready
        return Button(action: enableJIT) {
            if jitBusy {
                ProgressView()
            } else {
                Label(ready ? "JIT On" : "Enable JIT", systemImage: ready ? "bolt.fill" : "bolt")
                    .labelStyle(.titleAndIcon)
            }
        }
        .tint(ready ? .green : nil)
        .disabled(jitBusy || ready)
        .accessibilityLabel(ready ? "JIT is on" : "Enable JIT")
    }

    private func enableJIT() {
        jitBusy = true
        Task {
            let enabled = await jit.acquire(settings: settings)
            jitBusy = false
            if !enabled { jitMessage = jit.statusText + "\n\nDetails are in Settings › JIT log." }
        }
    }

    private var emptyState: some View {
        VStack(spacing: 14) {
            Image(systemName: "opticaldisc").font(.system(size: 52)).foregroundColor(.secondary)
            Text("No games yet").font(.title2.weight(.semibold))
            Text("Copy .iso / .xiso disc images, .xbe files or extracted-disc folders into "
                 + "Ember › Games in the Files app, or tap + to pick them where they are.")
                .font(.callout).foregroundColor(.secondary).multilineTextAlignment(.center)
                .frame(maxWidth: 420)
            Button("Add Games…") { showImporter = true }.buttonStyle(.borderedProminent)
        }
        .padding()
    }
}

private struct GameCard: View {
    let game: Game
    @State private var artwork: UIImage?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color(.secondarySystemBackground))
                if let artwork = artwork {
                    Image(uiImage: artwork).resizable().interpolation(.high).scaledToFill()
                } else {
                    // No cover in Documents/Covers for this ID: show the title instead.
                    VStack(spacing: 8) {
                        Image(systemName: "opticaldisc").font(.system(size: 34)).foregroundColor(.secondary)
                        Text(game.title).font(.caption.weight(.semibold)).multilineTextAlignment(.center)
                            .lineLimit(3).padding(.horizontal, 8)
                    }
                }
            }
            .aspectRatio(0.71, contentMode: .fit) // the proportions of a DVD case
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))

            Text(game.title).font(.subheadline.weight(.semibold)).lineLimit(2)
                .frame(maxWidth: .infinity, alignment: .leading)
            if !game.idLabel.isEmpty {
                Text(game.idLabel).font(.caption2.monospaced()).foregroundColor(.secondary).lineLimit(1)
            }
            HStack(spacing: 6) {
                Text(game.formatLabel)
                    .font(.caption2.weight(.bold)).padding(.horizontal, 6).padding(.vertical, 2)
                    .background(Color.accentColor.opacity(0.18)).cornerRadius(4)
                Text(game.sizeLabel).font(.caption).foregroundColor(.secondary)
                if !game.verified {
                    Image(systemName: "exclamationmark.triangle.fill").font(.caption).foregroundColor(.orange)
                        .accessibilityLabel("Not recognised as an Xbox disc image")
                }
            }
        }
        .task(id: game.id) {
            if let url = game.artworkURL { artwork = await ArtworkCache.shared.image(at: url) }
        }
        .accessibilityElement(children: .combine)
    }
}
