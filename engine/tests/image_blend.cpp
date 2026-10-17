// N_16a composes text and labels into transparent menu images.
#include <cstdio>
#include <filesystem>
#include <unistd.h>

#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/surface.h"
#include "platform/apple/apple_platform.h"

namespace ore { void register_image_op_natives(Engine &); }

int main() {
	char saves[] = "/tmp/ore-image-blend-XXXXXX";
	if (!mkdtemp(saves)) return 2;
	ore::ApplePlatform platform(saves);
	ore::Engine engine(platform, {});
	ore::register_image_op_natives(engine);
	auto dst = std::make_shared<ore::Image>(4, 1, 32);
	auto src = std::make_shared<ore::Image>(4, 1, 32);
	dst->fill(0);
	src->row32(0)[0] = 0xffffffff;
	src->row32(0)[1] = 0x80ffffff;
	src->row32(0)[2] = 0x00000000;
	src->row32(0)[3] = 0x80ff0000;
	dst->row32(0)[2] = 0x80445566;
	dst->row32(0)[3] = 0x800000ff;
	uint32_t dh = engine.own_image(std::make_unique<ore::ImageObj>(dst))->handle();
	uint32_t sh = engine.own_image(std::make_unique<ore::ImageObj>(src))->handle();
	uint32_t args = engine.vm.mem().alloc(16, "blend arguments");
	engine.vm.mem().wr32(args, dh);
	engine.vm.mem().wr32(args + 4, sh);
	engine.vm.mem().wr32(args + 8, 255);
	engine.vm.mem().wr32(args + 12, 0xffff);
	bool ok = true;
	auto check = [&](bool value, const char *message) {
		if (!value) { fprintf(stderr, "FAIL: %s\n", message); ok = false; }
	};
	check(engine.vm.native(0x16a)(engine.vm, args) == 0, "menu composition succeeds");
	check(dst->pixel(0, 0) == 0xffffffff, "opaque text remains visible on a transparent panel");
	check(dst->pixel(1, 0) == 0x80ffffff, "antialiasing retains straight colour and coverage");
	check(dst->pixel(2, 0) == 0x80445566, "transparent source leaves the panel unchanged");
	uint32_t p = dst->pixel(3, 0);
	check((p >> 24) >= 191 && (p >> 24) <= 192, "overlapping coverage accumulates");
	check(((p >> 16) & 255) >= 169 && (p & 255) >= 84, "overlap combines both colours");
	dst->fill(0);
	engine.vm.mem().wr32(args + 8, 128);
	engine.vm.native(0x16a)(engine.vm, args);
	check(dst->pixel(0, 0) == 0x80ffffff, "extra opacity scales coverage without darkening straight colour");
	ore::Image screen(1, 1, 32), glyph(1, 1, 32);
	screen.fill(0xff000000); glyph.fill(0x80ffffff);
	ore::blit(screen, glyph, 0, 0, glyph.rect(), ore::kBlendAlpha, 255, false);
	check(screen.pixel(0, 0) == 0x007f7f7f, "ordinary sprite blend keeps its existing arithmetic");
	if (ok) puts("image_blend: transparent menu text and sprite blending passed");
	std::filesystem::remove_all(saves);
	return ok ? 0 : 1;
}
