import Foundation

@main
struct FolderImportTests {
    static func checkGame(_ folder: URL) throws {
        let data = try Data(contentsOf: folder.appendingPathComponent("SCRIPT.LPK"))
        precondition(data == Data("game".utf8))
    }

    static func main() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? fm.removeItem(at: root) }
        let documents = root.appendingPathComponent("Documents")
        let source = root.appendingPathComponent("Source/SET_UP")
        try fm.createDirectory(at: documents, withIntermediateDirectories: true)
        try fm.createDirectory(at: source, withIntermediateDirectories: true)
        try Data("game".utf8).write(to: source.appendingPathComponent("SCRIPT.LPK"))
        try FolderImport.copy(source, into: documents)
        let imported = documents.appendingPathComponent("SET_UP")
        try checkGame(imported)
        // Selecting an already imported directory must preserve it.
        try FolderImport.copy(imported, into: documents)
        precondition(fm.fileExists(atPath: imported.appendingPathComponent("SCRIPT.LPK").path))
        // A name collision must never delete the existing installation.
        try Data("replacement".utf8).write(to: source.appendingPathComponent("SCRIPT.LPK"))
        do {
            try FolderImport.copy(source, into: documents)
            fatalError("Expected a name collision")
        } catch FolderImport.ImportError.alreadyExists { }
        try checkGame(imported)
        // Failure must not expose a partially imported folder.
        do {
            try FolderImport.copy(root.appendingPathComponent("missing"), into: documents)
            fatalError("Expected missing source failure")
        } catch { }
        let items = try fm.contentsOfDirectory(atPath: documents.path)
        precondition(items == ["SET_UP"])
        print("FolderImport: 4 regression scenarios passed")
    }
}
