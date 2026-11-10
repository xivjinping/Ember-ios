import Foundation
import ImageIO
import UIKit

struct Game: Identifiable, Hashable {
    let id: String          // file path
    let url: URL
    let title: String
    let format: TitleFormat
    /// False when the file only looks like an Xbox image by extension.
    let verified: Bool
    let titleID: UInt32
    let titleCode: String   // "MS-004" style, or ""
    let sizeBytes: Int64
    let artworkURL: URL?

    var formatLabel: String {
        switch format {
        case .xbe: return "XBE"
        case .xiso: return "ISO"
        case .folder: return "Folder"
        default: return "?"
        }
    }

    /// The unique ID shown under each game: publisher code plus the raw hex ID.
    var idLabel: String {
        guard titleID != 0 else { return "" }
        let hex = String(format: "%08X", titleID)
        return titleCode.isEmpty ? hex : "\(titleCode) · \(hex)"
    }

    var sizeLabel: String {
        ByteCountFormatter.string(fromByteCount: sizeBytes, countStyle: .file)
    }
}

/// The game library.
///
/// Games come from two places:
///   * `Documents/Games` inside the app's container — visible in the Files app
///     and Finder (UIFileSharingEnabled), so multi-gigabyte images can be
///     dropped in without being copied again.
///   * Files or folders picked with the document picker. These are not copied
///     either: a security-scoped bookmark is stored and the image is read in
///     place (LSSupportsOpeningDocumentsInPlace).
///
/// Every candidate is opened and parsed (see Core/src/fs/title_probe.cpp), so
/// names and title IDs come from the disc's own executable, not the filename.
///
/// Artwork: disc images carry no cover art. Put `<TITLEID>.png` or `.jpg`
/// (the 8-digit hex ID shown in the grid, e.g. `4D530004.jpg`) in
/// `Documents/Covers` and it is picked up on the next scan.
@MainActor
final class GameLibrary: ObservableObject {
    @Published private(set) var games: [Game] = []
    @Published private(set) var isScanning = false

    let gamesDirectory: URL
    let coversDirectory: URL
    private let defaults: UserDefaults
    private var scopedURLs: [URL] = []          // kept open for the app's lifetime
    private static let bookmarksKey = "library.bookmarks"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        gamesDirectory = documents.appendingPathComponent("Games", isDirectory: true)
        coversDirectory = documents.appendingPathComponent("Covers", isDirectory: true)
        try? FileManager.default.createDirectory(at: gamesDirectory, withIntermediateDirectories: true)
        try? FileManager.default.createDirectory(at: coversDirectory, withIntermediateDirectories: true)
        restoreBookmarks()
    }

    // MARK: Import

    /// Remembers items chosen in the document picker (files or whole folders).
    func addPicked(_ urls: [URL]) {
        var stored = defaults.array(forKey: Self.bookmarksKey) as? [Data] ?? []
        for url in urls {
            guard url.startAccessingSecurityScopedResource() else { continue }
            scopedURLs.append(url)
            if let bookmark = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
                stored.append(bookmark)
            }
        }
        defaults.set(stored, forKey: Self.bookmarksKey)
        Task { await rescan() }
    }

    private func restoreBookmarks() {
        let stored = defaults.array(forKey: Self.bookmarksKey) as? [Data] ?? []
        var kept: [Data] = []
        for data in stored {
            var stale = false
            guard let url = try? URL(resolvingBookmarkData: data, bookmarkDataIsStale: &stale),
                  url.startAccessingSecurityScopedResource() else { continue } // moved or deleted: forget it
            scopedURLs.append(url)
            kept.append(stale ? ((try? url.bookmarkData()) ?? data) : data)
        }
        defaults.set(kept, forKey: Self.bookmarksKey)
    }

    // MARK: Scan

    func rescan() async {
        guard !isScanning else { return }
        isScanning = true
        let roots = [gamesDirectory] + scopedURLs
        let covers = coversDirectory
        let found = await Task.detached(priority: .userInitiated) { GameLibrary.scan(roots: roots, covers: covers) }.value
        games = found
        isScanning = false
    }

    /// Runs off the main thread.
    nonisolated private static func scan(roots: [URL], covers: URL) -> [Game] {
        let fm = FileManager.default
        let extensions: Set<String> = ["iso", "xiso", "xbe"]
        var seen = Set<String>()
        var games: [Game] = []

        func artwork(for info: TitleInfo) -> URL? {
            guard info.titleID != 0 else { return nil }
            let hex = String(format: "%08X", info.titleID)
            for name in [hex, info.titleCode] where !name.isEmpty {
                for ext in ["png", "jpg", "jpeg"] {
                    let candidate = covers.appendingPathComponent("\(name).\(ext)")
                    if fm.fileExists(atPath: candidate.path) { return candidate }
                }
            }
            return nil
        }

        func consider(_ url: URL) -> Bool {
            guard !seen.contains(url.standardizedFileURL.path), let info = EmulatorCore.probeTitle(at: url) else { return false }
            seen.insert(url.standardizedFileURL.path)
            games.append(Game(id: url.path, url: url, title: info.name, format: info.format, verified: info.verified,
                              titleID: info.titleID, titleCode: info.titleCode,
                              sizeBytes: Int64(clamping: info.sizeBytes), artworkURL: artwork(for: info)))
            return true
        }

        func walk(_ directory: URL, depth: Int) {
            let keys: [URLResourceKey] = [.isDirectoryKey]
            guard let entries = try? fm.contentsOfDirectory(at: directory, includingPropertiesForKeys: keys,
                                                            options: [.skipsHiddenFiles]) else { return }
            for entry in entries {
                let isDirectory = (try? entry.resourceValues(forKeys: Set(keys)).isDirectory) ?? false
                if isDirectory {
                    // A folder holding default.xbe is an extracted disc: list it, don't descend.
                    if !consider(entry), depth < 3 { walk(entry, depth: depth + 1) }
                } else if extensions.contains(entry.pathExtension.lowercased()) {
                    _ = consider(entry)
                }
            }
        }

        for root in roots {
            var isDirectory: ObjCBool = false
            guard fm.fileExists(atPath: root.path, isDirectory: &isDirectory) else { continue }
            // A picked item is either a game itself (an image, or an extracted-
            // disc folder) or a folder of games.
            if consider(root) { continue }
            if isDirectory.boolValue { walk(root, depth: 0) }
        }
        return games.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
    }
}

/// Decodes and caches cover art off the main thread.
actor ArtworkCache {
    static let shared = ArtworkCache()
    private var cache: [URL: UIImage] = [:]

    func image(at url: URL) -> UIImage? {
        if let hit = cache[url] { return hit }
        // Decode a thumbnail rather than the full image: covers can be large scans.
        let options: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                        kCGImageSourceCreateThumbnailWithTransform: true,
                                        kCGImageSourceThumbnailMaxPixelSize: 512]
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
        let result = UIImage(cgImage: image)
        cache[url] = result
        return result
    }
}
