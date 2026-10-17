import Foundation

/// Publish a complete copy only after copying succeeds. Existing installations
/// remain untouched when a user chooses the same folder or a conflicting name.
enum FolderImport {
    enum ImportError: LocalizedError {
        case alreadyExists

        var errorDescription: String? {
            "已有同名文件夹。请先在「文件」App 中重命名新文件夹，再导入。"
        }
    }

    static func copy(_ source: URL, into documents: URL) throws {
        let fm = FileManager.default
        let target = documents.appendingPathComponent(source.lastPathComponent, isDirectory: true)
        if source.resolvingSymlinksInPath().standardizedFileURL == target.resolvingSymlinksInPath().standardizedFileURL {
            return
        }
        guard !fm.fileExists(atPath: target.path) else { throw ImportError.alreadyExists }
        let staging = documents.appendingPathComponent(".import-\(UUID().uuidString)", isDirectory: true)
        defer {
            if fm.fileExists(atPath: staging.path) {
                do { try fm.removeItem(at: staging) }
                catch { NSLog("Import staging cleanup failed: %@", error.localizedDescription) }
            }
        }
        try fm.copyItem(at: source, to: staging)
        try fm.moveItem(at: staging, to: target)
    }
}
