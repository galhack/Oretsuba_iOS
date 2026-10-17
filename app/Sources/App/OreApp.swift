import SwiftUI

@main
struct OreApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(model)
                .preferredColorScheme(.dark)
        }
        #if os(visionOS)
        .defaultSize(width: 1600, height: 1200)
        #endif
    }
}

struct RootView: View {
    @EnvironmentObject private var model: AppModel
    @StateObject private var controls = GameControls()

    var body: some View {
        if model.playing, let game = model.gameFolder {
            GameView(gameFolder: game,
                     patchFolder: model.effectiveLanguage == .chinese ? model.patchFolder : nil,
                     language: model.effectiveLanguage, controls: controls) { error in
                model.stop(error: error)
            }
            .ignoresSafeArea()
            .persistentSystemOverlays(.hidden)
            #if os(visionOS)
            .ornament(attachmentAnchor: .scene(.bottom), contentAlignment: .top) {
                VisionGameControls(controls: controls)
            }
            #endif
        } else {
            LauncherView()
        }
    }
}

/// Hosts the game's view controller.
struct GameView: UIViewControllerRepresentable {
    let gameFolder: URL
    let patchFolder: URL?
    let language: GameLanguage
    let controls: GameControls
    let onExit: (String?) -> Void

    func makeUIViewController(context: Context) -> GameViewController {
        let controller = GameViewController(gameFolder: gameFolder, patchFolder: patchFolder, language: language, onExit: onExit)
        controls.controller = controller
        return controller
    }

    func updateUIViewController(_ controller: GameViewController, context: Context) {}
}

/// Weak ownership lets native controls target the currently displayed game.
final class GameControls: ObservableObject {
    weak var controller: GameViewController?

    func send(_ command: GameCommand) { controller?.sendCommand(command) }
}

#if os(visionOS)
private struct VisionGameControls: View {
    let controls: GameControls

    var body: some View {
        HStack(spacing: 12) {
            action("保存", .save)
            action("读取", .load)
            action("快存", .quickSave)
            action("快读", .quickLoad)
            action("回想", .history)
            action("菜单", .menu)
        }
        .padding(16)
        .glassBackgroundEffect()
    }

    private func action(_ title: String, _ command: GameCommand) -> some View {
        Button { controls.send(command) } label: {
            Text(title).font(.title3).frame(minWidth: 72, minHeight: 60)
        }
        .buttonStyle(.borderless)
        .accessibilityLabel(title)
    }
}
#endif
