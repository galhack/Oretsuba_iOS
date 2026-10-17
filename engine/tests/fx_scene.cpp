// Visual check of transitions and layer effects with the game data: drives the
// natives directly on a front/back page pair and writes contact sheets.
//   fx_scene DATA_DIR OUT_DIR
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/transition.h"
#include "platform/apple/apple_platform.h"
#include "stb_image_write.h"

using namespace ore;

// Weak fallbacks so the harness links while other areas are being rewritten.
namespace ore {
__attribute__((weak)) void register_text_natives(Engine &) {}
__attribute__((weak)) void register_ui_natives(Engine &) {}
}  // namespace ore

namespace {

Engine *E;

uint32_t str(const char *s) {
	return E->vm.mem().alloc_string(s);
}

int32_t call(int idx, std::initializer_list<uint32_t> args) {
	static uint32_t base = 0;
	if (!base) base = E->vm.mem().alloc(64 * 4, "args");
	int i = 0;
	for (uint32_t v : args) E->vm.mem().wr32(base + uint32_t(i++) * 4, v);
	NativeFn fn = E->vm.native(idx);
	if (!fn) {
		std::printf("native %x missing\n", idx);
		return 0;
	}
	return fn(E->vm, base);
}

void save_png(const Image &img, const std::string &path) {
	std::vector<uint8_t> rgba(size_t(img.width()) * img.height() * 4);
	for (int y = 0; y < img.height(); ++y)
		for (int x = 0; x < img.width(); ++x) {
			uint32_t p = img.row32(y)[x];
			uint8_t *d = &rgba[(size_t(y) * img.width() + x) * 4];
			d[0] = uint8_t(p >> 16), d[1] = uint8_t(p >> 8), d[2] = uint8_t(p), d[3] = 255;
		}
	stbi_write_png(path.c_str(), img.width(), img.height(), 4, rgba.data(), img.width() * 4);
}

struct Sheet {
	std::vector<ImageRef> tiles;
	void add(const Image &img) {
		auto t = std::make_shared<Image>(200, 150, 32);
		stretch_blit(*t, img, 0, 0, 200, 150, img.rect(), 0, 2, 255, false);
		tiles.push_back(t);
	}
	void write(const std::string &path, int cols = 6) {
		int rows = int((tiles.size() + cols - 1) / cols);
		Image s(200 * cols, 150 * rows, 32);
		for (size_t i = 0; i < tiles.size(); ++i) copy_rect(s, int(i % cols) * 200, int(i / cols) * 150, *tiles[i], tiles[i]->rect());
		save_png(s, path);
		std::printf("wrote %s\n", path.c_str());
		tiles.clear();
	}
};

int64_t now_ms = 0;
Surface *front, *back;
uint32_t hf, hb;

// Runs `ms` of engine time, grabbing the front page every `every` ms.
void run(int ms, int every, Sheet &sh) {
	for (int t = 0; t <= ms; t += 16) {
		now_ms += 16;
		E->now = now_ms;
		E->tasks.tick(now_ms);
		back->update(now_ms);
		front->update(now_ms);
		if (front->transition && front->transition->done) front->end_transition(front->trans_apply);
		if (every && t % every < 16) sh.add(front->current_image());
	}
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 3) return 2;
	std::string out = argv[2];
	ApplePlatform platform("/tmp/fx_scene_save");
	EngineConfig cfg;
	cfg.data_dir = argv[1];
	Engine e(platform, cfg);
	E = &e;
	std::string err;
	if (!e.vfs.mount_archives(cfg.data_dir, &err)) {
		std::printf("mount: %s\n", err.c_str());
		return 1;
	}
	register_gfx_natives(e);
	register_effect_natives(e);

	hf = uint32_t(call(0xde, {800, 600, 32, 2}));
	hb = uint32_t(call(0xeb, {800, 600, 32, 1}));
	call(0xf9, {hf, hb});
	front = e.surface_of(hf);
	back = e.surface_of(hb);
	uint32_t bg1 = uint32_t(call(0xe4, {str("PIC\\BG01.ELG")}));
	uint32_t bg2 = uint32_t(call(0xe4, {str("PIC\\BG02A.ELG")}));
	uint32_t chr = uint32_t(call(0xe4, {str("CHR\\KAZ01B.ELG")}));
	std::printf("images: %x %x %x\n", bg1, bg2, chr);
	call(0x19d, {hf, bg1, 0, 0});
	call(0x19d, {hb, bg2, 0, 0});
	Sheet sh;

	// 1. rule transition (RULE040, vague 500) then a wave and a shutter
	uint32_t rule = uint32_t(call(0xe5, {str("SYS\\RULE040.ELG")}));
	std::printf("N_fb rule = %d\n", call(0xfb, {hf, str("rule"), 1, 1, 1000, rule, 500}));
	run(1000, 200, sh);
	call(0xfb, {hf, str("wave"), 1, 1, 1000, 3, 550, 768, 0, 0});
	run(1000, 200, sh);
	call(0xfb, {hf, str("quake"), 1, 0, 500, 30, 30, 0, str("0")});
	run(300, 100, sh);
	sh.write(out + "/scene_transitions.png");

	// 2. a character with sp_move: in from the left, shake, dip, out right with fade
	uint32_t spr = uint32_t(call(0x18d, {chr, 0xffff}));
	Image *ci = e.image_of(chr).get();
	std::printf("char %dx%d offset %d,%d\n", ci->width(), ci->height(), ci->offset_x, ci->offset_y);
	call(0x1b2, {spr, uint32_t(ci->offset_x), uint32_t(ci->offset_y)});
	call(0x194, {hf, spr, 2003, 0, 0});
	call(0xd9, {str("sp_move"), 0, 2000, 2006});
	uint32_t m = uint32_t(call(0xd6, {hf, str("sp_move"), spr, str("l"), 400}));
	run(500, 100, sh);
	m = uint32_t(call(0xd6, {hf, str("sp_move"), spr, str("$ $"), 300}));
	run(700, 100, sh);
	m = uint32_t(call(0xd6, {hf, str("sp_move"), spr, str("c_"), 300}));
	run(700, 100, sh);
	m = uint32_t(call(0xd6, {hf, str("sp_move"), spr, str("o!r"), 500}));
	run(600, 100, sh);
	(void)m;
	sh.write(out + "/scene_spmove.png");

	// 3. layer quake, bg_rast, bg_zoom, bg_blur
	call(0x1b1, {spr, 255});
	call(0x1b2, {spr, uint32_t(ci->offset_x), uint32_t(ci->offset_y)});
	call(0xd6, {hf, str("quake"), 300, 0, 16, 16, str("0"), 0, 1, 1, 1});
	run(320, 80, sh);
	uint32_t ra = uint32_t(call(0xd6, {hf, str("bg_rast"), 20, 200, 1000, 0, 0, 0, 0, 0}));
	run(400, 200, sh);
	call(0xd0, {[&] { uint32_t p = e.vm.mem().alloc(4); e.vm.mem().wr32(p, ra); return p; }()});
	call(0xd6, {hf, str("bg_zoom"), 400, 300, str("2.0"), str("0"), 600});
	run(700, 200, sh);
	call(0x1a0, {hf, 0, 0, 0, 0, 0xffff});
	call(0xd6, {hf, str("bg_blur"), 0, 400, 300, str("1.0"), str("0"), str("1.5"), str("0"), 500, 64, str("0"), 0, 0, 0, 0, 0, 0, 1});
	run(600, 120, sh);
	sh.write(out + "/scene_bgfx.png");

	// 4. weather overlays
	const char *weather[] = {"snow", "rain", "paper", "light"};
	for (const char *w : weather) {
		uint32_t h = uint32_t(call(0xd6, {hf, str(w), 60, 1993, 100, 1, 1, 0}));
		run(1500, 500, sh);
		save_png(front->current_image(), out + "/weather_" + w + ".png");
		uint32_t p = e.vm.mem().alloc(4);
		e.vm.mem().wr32(p, h);
		call(0xd0, {p});
		run(32, 0, sh);
	}
	uint32_t h = uint32_t(call(0xd6, {hf, str("noise"), 96, 1993, 1, 0}));
	run(300, 150, sh);
	(void)h;
	sh.write(out + "/scene_weather.png");

	// 5. image filters on the character image (gray, sepia, invert, darken, blur, flip)
	auto ops = [&](std::initializer_list<uint32_t> args, int idx) {
		uint32_t c = uint32_t(call(0xe9, {chr}));
		std::vector<uint32_t> v(args);
		v.insert(v.begin(), c);
		static uint32_t base = e.vm.mem().alloc(64 * 4);
		for (size_t i = 0; i < v.size(); ++i) e.vm.mem().wr32(base + uint32_t(i) * 4, v[i]);
		e.vm.native(idx)(e.vm, base);
		Image bgimg(800, 600, 32);
		fill_rect(bgimg, bgimg.rect(), 0x808080);
		ImageRef im = e.image_of(c);
		blit(bgimg, *im, 0, 0, im->rect(), 2, 255, false);
		sh.add(bgimg);
	};
	ops({0xffff}, 0x175);
	ops({152, 128, 107, 24, 0, uint32_t(-4), 0xffff}, 0x176);
	ops({0, 0xffff}, 0x177);
	ops({128, 0xffff}, 0x15a);
	ops({8, 8, 0, 0xffff}, 0x185);
	ops({0xffff}, 0x187);
	sh.write(out + "/scene_imageops.png");
	return 0;
}
