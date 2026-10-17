import AVFoundation
import Foundation

/// Owns the engine instance and its audio output.
final class EngineBridge {
    let engine: OpaquePointer
    private let audio = AVAudioEngine()
    private var source: AVAudioSourceNode?
    private var diagnosticSaves: URL?

    init(gameFolder: URL, patchFolder: URL?, language: GameLanguage) throws {
        var saves = GamePaths.saves(for: language)
        #if DEBUG
        let movieTest = ProcessInfo.processInfo.environment["ORE_TEST_MOVIE"].flatMap(Int.init)
        if let index = movieTest, (0...1).contains(index) {
            saves = FileManager.default.temporaryDirectory.appendingPathComponent("OreMovieTest-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: saves, withIntermediateDirectories: true)
            diagnosticSaves = saves
        }
        #endif
        var err = [CChar](repeating: 0, count: 512)
        let created: OpaquePointer? = gameFolder.path.withCString { data in
            saves.path.withCString { save in
                let make: (UnsafePointer<CChar>?) -> OpaquePointer? = { patch in
                    var cfg = OreConfig(data_dir: data, patch_dir: patch, save_dir: save,
                                        chinese: language == .chinese ? 1 : 0)
                    return ore_create(&cfg, &err, Int32(err.count))
                }
                if let patch = patchFolder { return patch.path.withCString { make($0) } }
                return make(nil)
            }
        }
        guard let created else {
            throw NSError(domain: "Oretsuba", code: 1, userInfo: [NSLocalizedDescriptionKey: String(cString: err)])
        }
        #if DEBUG
        if let index = movieTest, diagnosticSaves != nil, ore_debug_start_movie(created, Int32(index)) == 0 {
            ore_destroy(created)
            throw NSError(domain: "Oretsuba", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "无法启动片头播放测试"])
        }
        #endif
        engine = created
        startAudio()
    }

    deinit {
        audio.stop()
        ore_destroy(engine)
        if let diagnosticSaves {
            do { try FileManager.default.removeItem(at: diagnosticSaves) }
            catch { fputs("movie diagnostic cleanup: \(error)\n", stderr) }
        }
    }

    private func startAudio() {
        let session = AVAudioSession.sharedInstance()
        // Playback: the game is heard even with the silent switch on.
        try? session.setCategory(.playback, mode: .default)
        try? session.setActive(true)
        let rate = Double(ore_audio_rate())
        guard let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2) else { return }
        let engine = self.engine
        var scratch = [Float](repeating: 0, count: 4096 * 2)
        let node = AVAudioSourceNode(format: format) { _, _, frameCount, bufferList -> OSStatus in
            let frames = Int(frameCount)
            if scratch.count < frames * 2 { scratch = [Float](repeating: 0, count: frames * 2) }
            scratch.withUnsafeMutableBufferPointer { ore_audio_mix(engine, $0.baseAddress, Int32(frames)) }
            // non-interleaved stereo: one buffer per channel
            let count = Int(bufferList.pointee.mNumberBuffers)
            guard count >= 2 else { return noErr }
            withUnsafeMutablePointer(to: &bufferList.pointee.mBuffers) { first in
                let buffers = UnsafeMutableBufferPointer(start: first, count: count)
                guard let left = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                      let right = buffers[1].mData?.assumingMemoryBound(to: Float.self) else { return }
                for i in 0..<frames {
                    left[i] = scratch[i * 2]
                    right[i] = scratch[i * 2 + 1]
                }
            }
            return noErr
        }
        audio.attach(node)
        audio.connect(node, to: audio.mainMixerNode, format: format)
        source = node
        do {
            try audio.start()
        } catch {
            fputs("audio: \(error)\n", stderr)
        }
    }

    func setPaused(_ paused: Bool) {
        ore_set_paused(engine, paused ? 1 : 0)
        if paused { audio.pause() } else { try? audio.start() }
    }
}
