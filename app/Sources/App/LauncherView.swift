import SwiftUI
import UniformTypeIdentifiers

/// The first screen: whether the game files are there, the language, and how
/// to bring the files onto this device.
struct LauncherView: View {
    @EnvironmentObject private var model: AppModel
    @State private var picking: PickTarget = .game
    @State private var showingImporter = false

    enum PickTarget {
        case game, patch
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("我们没有翅膀").font(.largeTitle.bold())
                    Text("俺たちに翼はない · Apple 平台移植").foregroundStyle(.secondary)
                }
                statusCard
                if model.gameFolder != nil {
                    Picker("语言", selection: $model.language) {
                        ForEach(GameLanguage.allCases) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .disabled(!model.hasChinesePatch)
                    Button {
                        model.start()
                    } label: {
                        Text("开始游戏").font(.title2.bold()).frame(maxWidth: .infinity).padding(.vertical, 8)
                    }
                    .buttonStyle(.borderedProminent)
                }
                importCard
                if let error = model.errorMessage {
                    Text(error).foregroundStyle(.red)
                }
                controlsHelp
            }
            .padding(.horizontal, 40)
            .padding(.vertical, 36)
            .frame(maxWidth: 860, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
        .onAppear { model.refresh() }
        .fileImporter(isPresented: $showingImporter,
                      allowedContentTypes: [.folder]) { result in
            // Dismissal can set isPresented to false before this callback.
            // Keep the selected target independently until the result arrives.
            guard case .success(let url) = result else { return }
            #if targetEnvironment(macCatalyst)
            if picking == .game { model.choose(gameFolder: url) } else { model.choose(patchFolder: url) }
            #else
            model.importFolder(url)
            #endif
        }
    }

    private var statusCard: some View {
        GroupBox("游戏数据") {
            VStack(alignment: .leading, spacing: 8) {
                if let folder = model.gameFolder {
                    Label("已找到游戏文件", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    Text(folder.path).font(.footnote).foregroundStyle(.secondary).lineLimit(2)
                } else {
                    Label("尚未找到游戏文件", systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    Text("请放入你自己购买的游戏光盘中的 SET_UP 文件夹（含 SCRIPT.LPK、PIC.LPK 等文件）。")
                        .foregroundStyle(.secondary)
                }
                if let patch = model.patchFolder {
                    Label("已找到汉化补丁", systemImage: "character.book.closed.fill").foregroundStyle(.green)
                    Text(patch.path).font(.footnote).foregroundStyle(.secondary).lineLimit(2)
                } else {
                    Label("未找到汉化补丁（可选：含 SCRIPT_CHS 的文件夹）", systemImage: "character.book.closed")
                        .foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var importCard: some View {
        GroupBox(model.gameFolder == nil ? "导入游戏文件" : "管理文件") {
            VStack(alignment: .leading, spacing: 12) {
                #if targetEnvironment(macCatalyst)
                Text("选择电脑上的游戏目录（含 SCRIPT.LPK）与汉化补丁目录（含 SCRIPT_CHS），无需复制。")
                HStack {
                    Button("选择游戏目录…") { picking = .game; showingImporter = true }
                    Button("选择汉化补丁目录…") { picking = .patch; showingImporter = true }
                }
                #else
                Text("方式一：用数据线连接 Mac，在访达中选中本设备 › 文件 › 我们没有翅膀，把 SET_UP 文件夹和汉化补丁文件夹拖进去。")
                Text("方式二：先用隔空投送或其他方式把文件夹存入「文件」App，再点下方按钮导入。")
                Button("从「文件」导入文件夹…") { picking = .game; showingImporter = true }
                    .disabled(model.importing)
                if model.importing {
                    ProgressView(model.importProgress)
                }
                #endif
                Button("重新检查") { model.refresh() }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var controlsHelp: some View {
        VStack(alignment: .leading, spacing: 4) {
            #if targetEnvironment(macCatalyst)
            Text("操作同 PC 版：鼠标左键 / 右键（菜单）/ 滚轮（回想），按住 Ctrl 快进。")
            #else
            Text("操作：轻点 = 左键 · 双指轻点 = 右键（菜单）· 上下滑动 = 滚轮（回想）· 长按 = 快进。")
            Text("也可连接鼠标、触控板或键盘，用法同 PC 版。")
            #endif
        }
        .font(.footnote)
        .foregroundStyle(.secondary)
    }
}
