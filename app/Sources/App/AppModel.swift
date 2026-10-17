import Foundation
import SwiftUI

/// The app's state: where the files are, the chosen language, and whether
/// the game is running.
@MainActor
final class AppModel: ObservableObject {
    @Published var gameFolder: URL?
    @Published var patchFolder: URL?
    @Published var playing = false
    @Published var errorMessage: String?
    @Published var importing = false
    @Published var importProgress = ""

    @Published var language: GameLanguage {
        didSet { UserDefaults.standard.set(language.rawValue, forKey: "language") }
    }

    init() {
        let saved = UserDefaults.standard.string(forKey: "language").flatMap(GameLanguage.init(rawValue:))
        language = saved ?? .chinese
        refresh()
        if ProcessInfo.processInfo.environment["ORE_AUTOSTART"] != nil, gameFolder != nil {
            playing = true
        }
    }

    var hasChinesePatch: Bool { patchFolder != nil }

    /// The language actually played: Chinese needs the patch.
    var effectiveLanguage: GameLanguage {
        language == .chinese && hasChinesePatch ? .chinese : .japanese
    }

    func refresh() {
        gameFolder = GamePaths.findGameFolder()
        patchFolder = GamePaths.findPatchFolder()
    }

    func start() {
        errorMessage = nil
        guard gameFolder != nil else { return }
        playing = true
    }

    func stop(error: String?) {
        playing = false
        errorMessage = error
    }

    #if targetEnvironment(macCatalyst)
    func choose(gameFolder url: URL) {
        GamePaths.chosenGameFolder = url
        refresh()
        if gameFolder == nil { errorMessage = "所选目录中没有找到 SCRIPT.LPK。" }
    }

    func choose(patchFolder url: URL) {
        GamePaths.chosenPatchFolder = url
        refresh()
        if patchFolder == nil { errorMessage = "所选目录中没有找到 SCRIPT_CHS 文件夹。" }
    }
    #else
    /// Copies a folder picked in the Files app into Documents.
    func importFolder(_ url: URL) {
        importing = true
        importProgress = "正在复制 \(url.lastPathComponent)…"
        let documents = GamePaths.documents
        Task.detached {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let failure: String?
            do {
                try FolderImport.copy(url, into: documents)
                failure = nil
            } catch {
                failure = "复制失败：\(error.localizedDescription)"
            }
            await MainActor.run {
                self.importing = false
                self.importProgress = ""
                self.errorMessage = failure
                self.refresh()
            }
        }
    }
    #endif
}
