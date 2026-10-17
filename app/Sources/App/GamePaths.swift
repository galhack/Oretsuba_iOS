import Foundation

/// Where the port finds the player's game files and keeps its own.
///
/// * iPhone, iPad, Vision Pro: the game folder (the DVD's SET_UP folder, or
///   the whole disc) and the optional Chinese patch folder are copied into
///   the app's Documents (Finder file sharing, the Files app, AirDrop, or the
///   in-app import). Saves live in Application Support.
/// * Mac: the player points at the folders where they are; nothing is copied.
enum GamePaths {
    static let fileManager = FileManager.default

    static var documents: URL {
        fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    static var applicationSupport: URL {
        var url = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        #if targetEnvironment(macCatalyst)
        url = url.appendingPathComponent("Oretsuba", isDirectory: true)
        #endif
        try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// Saves and settings, one folder per language (the Chinese scripts are
    /// different files, so their saves cannot be shared with the original's).
    static func saves(for language: GameLanguage) -> URL {
        let url = applicationSupport.appendingPathComponent("Saves/\(language.rawValue)", isDirectory: true)
        try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// Folders the player picked on the Mac (security-scoped bookmarks are not
    /// needed: Catalyst apps here are not sandboxed).
    static var chosenGameFolder: URL? {
        get { UserDefaults.standard.string(forKey: "gameFolderPath").map { URL(fileURLWithPath: $0, isDirectory: true) } }
        set { UserDefaults.standard.set(newValue?.path, forKey: "gameFolderPath") }
    }

    static var chosenPatchFolder: URL? {
        get { UserDefaults.standard.string(forKey: "patchFolderPath").map { URL(fileURLWithPath: $0, isDirectory: true) } }
        set { UserDefaults.standard.set(newValue?.path, forKey: "patchFolderPath") }
    }

    /// The folder holding SCRIPT.LPK, searched a few levels deep.
    static func findGameFolder() -> URL? {
        var roots: [URL] = []
        if let chosen = chosenGameFolder { roots.append(chosen) }
        #if !targetEnvironment(macCatalyst)
        roots.append(documents)
        #endif
        for root in roots {
            if let found = search(root, depth: 3, where: { hasFile($0, named: "SCRIPT.LPK") }) { return found }
        }
        return nil
    }

    /// The Chinese patch: a folder holding SCRIPT_CHS.
    static func findPatchFolder() -> URL? {
        var roots: [URL] = []
        if let chosen = chosenPatchFolder { roots.append(chosen) }
        #if !targetEnvironment(macCatalyst)
        roots.append(documents)
        #endif
        for root in roots {
            if let found = search(root, depth: 3, where: { hasDirectory($0, named: "SCRIPT_CHS") }) { return found }
        }
        return nil
    }

    private static func search(_ dir: URL, depth: Int, where match: (URL) -> Bool) -> URL? {
        if match(dir) { return dir }
        guard depth > 0, let items = try? fileManager.contentsOfDirectory(
            at: dir, includingPropertiesForKeys: [.isDirectoryKey], options: [.skipsHiddenFiles]) else { return nil }
        for item in items.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let isDir = (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            if isDir, let found = search(item, depth: depth - 1, where: match) { return found }
        }
        return nil
    }

    private static func names(in dir: URL) -> [String] {
        ((try? fileManager.contentsOfDirectory(atPath: dir.path)) ?? []).map { $0.uppercased() }
    }

    private static func hasFile(_ dir: URL, named name: String) -> Bool {
        names(in: dir).contains(name)
    }

    private static func hasDirectory(_ dir: URL, named name: String) -> Bool {
        guard let items = try? fileManager.contentsOfDirectory(atPath: dir.path) else { return false }
        return items.contains { item in
            var isDir: ObjCBool = false
            return item.uppercased() == name
                && fileManager.fileExists(atPath: dir.appendingPathComponent(item).path, isDirectory: &isDir) && isDir.boolValue
        }
    }
}

enum GameLanguage: String, CaseIterable, Identifiable {
    case chinese = "chs"
    case japanese = "jpn"

    var id: String { rawValue }
    var title: String {
        switch self {
        case .chinese: return "简体中文（汉化补丁）"
        case .japanese: return "日本語（原版）"
        }
    }
}
