import CoreGraphics
import Metal
import MetalKit

/// Runs the engine once per display frame and draws its 800x600 picture into
/// the MTKView, scaled to fit with black bars.
final class Renderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let linear: MTLSamplerState
    private var texture: MTLTexture?
    private var serial: UInt64 = .max
    private var needsRedraw = true

    var bridge: EngineBridge?
    /// Called once when the engine stops (nil error = the game quit normally).
    var onStop: ((String?) -> Void)?
    private var stopped = false

    private(set) var gameSize = CGSize(width: 800, height: 600)
    /// The picture's rectangle in the view, in points (for mapping input).
    private(set) var contentRect = CGRect.zero

    init?(view: MTKView) {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(),
              let library = try? device.makeLibrary(source: Self.shaderSource, options: nil) else { return nil }
        self.device = device
        self.queue = queue
        view.device = device
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.preferredFramesPerSecond = 60
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = library.makeFunction(name: "quad_vertex")
        desc.fragmentFunction = library.makeFunction(name: "quad_fragment")
        desc.colorAttachments[0].pixelFormat = view.colorPixelFormat
        guard let state = try? device.makeRenderPipelineState(descriptor: desc) else { return nil }
        pipeline = state
        let samp = MTLSamplerDescriptor()
        samp.minFilter = .linear
        samp.magFilter = .linear
        samp.sAddressMode = .clampToEdge
        samp.tAddressMode = .clampToEdge
        guard let sampler = device.makeSamplerState(descriptor: samp) else { return nil }
        linear = sampler
        super.init()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        needsRedraw = true
    }

    /// A view point to game pixels, clamped to the picture.
    func gamePoint(from viewPoint: CGPoint) -> CGPoint {
        guard contentRect.width > 0, contentRect.height > 0 else { return .zero }
        let x = (viewPoint.x - contentRect.minX) / contentRect.width * gameSize.width
        let y = (viewPoint.y - contentRect.minY) / contentRect.height * gameSize.height
        return CGPoint(x: min(max(x, 0), gameSize.width - 1), y: min(max(y, 0), gameSize.height - 1))
    }

    func draw(in view: MTKView) {
        guard let bridge else { return }
        if !stopped {
            let now = Int64(CACurrentMediaTime() * 1000)
            let result = ore_frame(bridge.engine, now)
            if result != 0 {
                stopped = true
                let message = result < 0 ? String(cString: ore_last_error(bridge.engine)) : nil
                DispatchQueue.main.async { self.onStop?(message) }
            }
        }
        let uploaded = upload(bridge)
        let scale = view.contentScaleFactor
        let drawable = view.drawableSize
        let fit = Self.fitRect(content: gameSize, in: drawable)
        contentRect = CGRect(x: fit.minX / scale, y: fit.minY / scale, width: fit.width / scale, height: fit.height / scale)
        guard uploaded || needsRedraw, let tex = texture else { return }
        guard let pass = view.currentRenderPassDescriptor, let target = view.currentDrawable,
              let commands = queue.makeCommandBuffer(),
              let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return }
        needsRedraw = false
        encoder.setRenderPipelineState(pipeline)
        var ndc = SIMD4<Float>(Float(fit.minX / drawable.width * 2 - 1), Float(1 - fit.minY / drawable.height * 2),
                               Float(fit.maxX / drawable.width * 2 - 1), Float(1 - fit.maxY / drawable.height * 2))
        encoder.setVertexBytes(&ndc, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
        encoder.setFragmentTexture(tex, index: 0)
        encoder.setFragmentSamplerState(linear, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
        commands.present(target)
        commands.commit()
    }

    private func upload(_ bridge: EngineBridge) -> Bool {
        let now = ore_frame_serial(bridge.engine)
        guard now != serial else { return false }
        var w: Int32 = 0, h: Int32 = 0
        guard let pixels = ore_framebuffer(bridge.engine, &w, &h), w > 0, h > 0 else { return false }
        if texture?.width != Int(w) || texture?.height != Int(h) {
            let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: Int(w), height: Int(h),
                                                                mipmapped: false)
            desc.usage = .shaderRead
            texture = device.makeTexture(descriptor: desc)
            gameSize = CGSize(width: Int(w), height: Int(h))
        }
        texture?.replace(region: MTLRegionMake2D(0, 0, Int(w), Int(h)), mipmapLevel: 0, withBytes: pixels,
                         bytesPerRow: Int(w) * 4)
        serial = now
        return true
    }

    /// The largest rectangle of the content's aspect inside `size`, centred.
    static func fitRect(content: CGSize, in size: CGSize) -> CGRect {
        guard content.width > 0, content.height > 0, size.width > 0, size.height > 0 else { return .zero }
        let scale = min(size.width / content.width, size.height / content.height)
        let w = (content.width * scale).rounded(), h = (content.height * scale).rounded()
        return CGRect(x: ((size.width - w) / 2).rounded(), y: ((size.height - h) / 2).rounded(), width: w, height: h)
    }

    /// Compiled at startup, so building needs no Metal toolchain.
    private static let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct QuadOut { float4 position [[position]]; float2 uv; };

// rect = (left, top, right, bottom) in normalised device coordinates
vertex QuadOut quad_vertex(uint vid [[vertex_id]], constant float4& rect [[buffer(0)]])
{
	float2 uv = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
	QuadOut o;
	o.position = float4(mix(rect.x, rect.z, uv.x), mix(rect.y, rect.w, uv.y), 0.0, 1.0);
	o.uv = uv;
	return o;
}

fragment float4 quad_fragment(QuadOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
	return float4(tex.sample(smp, in.uv).rgb, 1.0);
}
"""
}
