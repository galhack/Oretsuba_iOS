// Unit checks of the effect math (docs/re/30_effects.md).
// Build: clang++ -std=c++17 -Isrc tests/fx_test.cpp <libore.a> <frameworks>
#include <cstdio>
#include <cstring>

#include "gfx/effects/fx_util.h"
#include "gfx/effects/move_lang.h"
#include "gfx/effects/transitions.h"
#include "gfx/blend.h"
#include "gfx/transition.h"
#include "stb_image_write.h"
#include "vm/vm.h"

using namespace ore;

static int failures = 0;
#define CHECK(c)                                                       \
	do {                                                               \
		if (!(c)) {                                                    \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			++failures;                                                \
		}                                                              \
	} while (0)

static void test_tables() {
	CHECK(fx::icos(0) == 0x10000);
	CHECK(fx::icos(256) == -0x10000);
	CHECK(fx::icos(128) == 0 && fx::icos(384) == 0);
	CHECK(fx::icos((128 - 128) & 511) == 0x10000);  // sin(90°)
	CHECK(fx::ease_ms(500, 1000, 0) == 500);
	CHECK(fx::ease_ms(500, 1000, 2) == 250);
	CHECK(fx::ease_ms(500, 1000, -2) == 750);
}

static void test_moves() {
	fx::MoveContext c;
	c.sprite = true;
	c.W = 800, c.H = 600, c.iw = 300, c.ih = 500, c.home_x = 250, c.home_y = 100, c.cur_x = 250, c.cur_y = 100;
	fx::MoveProgram p;
	CHECK(fx::parse_moves("l", c, p) == 0);
	CHECK(p.steps.size() == 1 && p.final_x == 250 - 200 && p.final_y == 100);
	CHECK(fx::parse_moves("$ $", c, p) == 0);
	CHECK(p.steps.size() == 2 && p.steps[0].dw == 50 && p.steps[0].xoff == -25 && p.final_x == 250);
	CHECK(fx::parse_moves("r>2_", c, p) == 0);
	CHECK(p.steps.size() == 1 && p.steps[0].dx == 200 + 50 && p.steps[0].dh == 50);
	CHECK(fx::parse_moves("o!r", c, p) == 0);
	CHECK(p.steps.size() == 2 && p.steps[0].op == fx::MoveStep::kFadeOut && p.final_x == 800 && p.final_opacity == 0);
	CHECK(fx::parse_moves("a0 c-6", c, p) == 0);
	CHECK(p.steps.size() == 2 && p.steps[0].op == fx::MoveStep::kEase && p.steps[0].ease == 0.0);
	CHECK(p.steps[1].dy == -150 && p.final_y == -50);
	CHECK(fx::parse_moves("500 l", c, p) == 0);
	CHECK(p.steps[0].op == fx::MoveStep::kDuration && p.steps[0].value == 500);
	CHECK(fx::parse_moves("", c, p) == 5);

	// background: "r" pans to the right edge of the image
	fx::MoveContext b;
	b.W = 800, b.H = 600, b.iw = 1600, b.ih = 600;
	CHECK(fx::parse_moves("a0 r", b, p) == 0 && p.final_x == 800 && p.final_y == 0);

	// bounces: '$' shakes left 25, back, right 25, back
	fx::MoveStep st;
	st.dw = 50, st.xoff = -25;
	int ox, oy;
	fx::move_offset(st, 250, 1000, false, ox, oy);
	CHECK(ox == -25 && oy == 0);
	fx::move_offset(st, 500, 1000, false, ox, oy);
	CHECK(ox == 0);
	fx::move_offset(st, 750, 1000, false, ox, oy);
	CHECK(ox == 25);
	fx::move_offset(st, 1000, 1000, true, ox, oy);
	CHECK(ox == 0);
	// '_' dips down 50 and back
	fx::MoveStep dn;
	dn.dh = 50;
	fx::move_offset(dn, 500, 1000, true, ox, oy);
	CHECK(oy == 50);
	// a plain move reaches its end exactly with the cosine curve
	fx::MoveStep mv;
	mv.dx = 123, mv.dy = -77;
	fx::move_offset(mv, 300, 300, true, ox, oy);
	CHECK(ox == 123 && oy == -77);
	fx::move_offset(mv, 150, 300, true, ox, oy);
	CHECK(ox == 61 && oy == -39);
}

static void test_rule() {
	Image s1(4, 1, 32), s2(4, 1, 32), out(4, 1, 32), rule(4, 1, 8);
	for (int x = 0; x < 4; ++x) {
		s1.row32(0)[x] = 0x000000;
		s2.row32(0)[x] = 0xffffff;
		rule.row8(0)[x] = uint8_t(x * 80);  // 0, 80, 160, 240
	}
	// vague 0: phase = a; pixels with rule < phase show s2
	rule_blend(out, s1, s2, rule, 128, 0, 0, 0, 4, 1, 0, 0, 0, 0, 0, 0);
	CHECK((out.row32(0)[0] & 0xffffff) == 0xffffff);
	CHECK((out.row32(0)[1] & 0xffffff) == 0xffffff);
	CHECK((out.row32(0)[2] & 0xffffff) == 0x000000);
	// a == 255 copies s2
	rule_blend(out, s1, s2, rule, 255, 64, 0, 0, 4, 1, 0, 0, 0, 0, 0, 0);
	CHECK((out.row32(0)[3] & 0xffffff) == 0xffffff);
	// soft edge: blended value between
	rule_blend(out, s1, s2, rule, 128, 200, 0, 0, 4, 1, 0, 0, 0, 0, 0, 0);
	uint32_t v = out.row32(0)[1] & 0xff;
	CHECK(v > 0 && v < 255);
}

static void test_blur_mix() {
	Image a(5, 1, 32), b(5, 1, 32);
	uint32_t px[5] = {0, 0, 0xff, 0, 0};
	for (int i = 0; i < 5; ++i) a.row32(0)[i] = px[i];
	fx::hblur(b, a, 1);
	CHECK((b.row32(0)[2] & 0xff) == 85);  // (255*21845 + 0x8000) >> 16
	CHECK((b.row32(0)[1] & 0xff) == 85);
	CHECK((b.row32(0)[0] & 0xff) == 0);
	CHECK(fx::mix(0x000000, 0xffffff, 0) == 0);
	CHECK((fx::mix(0x000000, 0x0000ff, 128) & 0xff) == 127);
}

// Contact sheet: every transition at 0/25/50/75/100 % (rows), written to argv[1].
static void contact_sheet(const char *path) {
	const int W = 800, H = 600, TW = 160, TH = 120;
	Image from(W, H, 32), to(W, H, 32);
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			from.row32(y)[x] = 0xff000000u | (uint32_t(x * 255 / W) << 16) | (uint32_t(y * 255 / H) << 8) |
			                   (((x / 40 + y / 40) & 1) ? 0x60 : 0x20);
			to.row32(y)[x] = 0xff000000u | (((x / 100 + y / 100) & 1) ? 0x2050f0 : 0xf0e030);
		}
	auto rule = std::make_shared<Image>(W, H, 8);
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) rule->row8(y)[x] = uint8_t((x + y) * 255 / (W + H));
	struct T {
		const char *name;
		std::vector<int32_t> args;
	};
	std::vector<T> list = {{"cross", {1000}},          {"rule", {1000, 0, 64}},       {"scroll", {1000, 0, 2}},
	                       {"wave", {1000, 3, 550, 768, 0, 0}}, {"mosaic", {1000, 0, 32, 0}}, {"rotaxis", {1000, 0}},
	                       {"trans", {1000, 86, 0}},   {"zoomflip", {1000, 4, 0}},  {"zoomflip", {1000, 6, 0}},
	                       {"rotcenter", {1000, 0, 0}}, {"addsub", {1000, 0}},      {"hblur", {1000, 0, 32}},
	                       {"hblur", {1000, 1, 32}}};
	Vm vm([](const std::string &, std::vector<uint8_t> &) { return false; });
	uint32_t base = vm.mem().alloc(64 * 4, "args");
	const int cols = 5;
	Image sheet(TW * cols, TH * int(list.size()), 32);
	Image out(W, H, 32);
	for (size_t r = 0; r < list.size(); ++r) {
		for (size_t i = 0; i < list[r].args.size(); ++i) vm.mem().wr32(base + uint32_t(i) * 4, uint32_t(list[r].args[i]));
		Args a(vm, base);
		auto t = make_transition(list[r].name, a, 0, rule);
		t->start = 0;
		for (int c = 0; c < cols; ++c) {
			t->render(c * 250, from, to, out);
			stretch_blit(sheet, out, c * TW, int(r) * TH, TW, TH, out.rect(), 0, 0, 255, false);
		}
	}
	std::vector<uint8_t> rgba(size_t(sheet.width()) * sheet.height() * 4);
	for (int y = 0; y < sheet.height(); ++y)
		for (int x = 0; x < sheet.width(); ++x) {
			uint32_t p = sheet.row32(y)[x];
			uint8_t *d = &rgba[(size_t(y) * sheet.width() + x) * 4];
			d[0] = uint8_t(p >> 16), d[1] = uint8_t(p >> 8), d[2] = uint8_t(p), d[3] = 255;
		}
	stbi_write_png(path, sheet.width(), sheet.height(), 4, rgba.data(), sheet.width() * 4);
	std::printf("wrote %s\n", path);
}

int main(int argc, char **argv) {
	if (argc > 1) contact_sheet(argv[1]);
	test_tables();
	test_moves();
	test_rule();
	test_blur_mix();
	std::printf(failures ? "%d failure(s)\n" : "all fx tests passed\n", failures);
	return failures ? 1 : 0;
}
