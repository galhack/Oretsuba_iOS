// Image-processing natives N_13b..N_18c (docs/re/30_effects.md §6).
#include <algorithm>
#include <cstring>
#include <functional>

#include "core/args.h"
#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/effects/fx_util.h"
#include "gfx/effects/transitions.h"
#include "util/log.h"

namespace ore {
namespace {

// Four ints l,t,r,b at args[i]; 0xffff in the first = whole image. Clipped to the image.
Rect rect_at(Args &a, int i, const Image &img) {
	if (a.u(i) == 0xffff) return img.rect();
	return Rect(a.i(i), a.i(i + 1), a.i(i + 2), a.i(i + 3)).intersect(img.rect());
}

// Per-channel op over the B,G,R bytes of 32 bpp pixels, or the palette of an 8 bpp image.
void map_rgb(Image &img, const Rect &r, const std::function<uint32_t(uint32_t)> &fn) {
	if (img.bpp() == 8) {
		for (int i = 0; i < 256; ++i) img.palette()[i] = fn(img.palette()[i]);
		return;
	}
	for (int y = r.t; y < r.b; ++y) {
		uint32_t *p = img.row32(y);
		for (int x = r.l; x < r.r; ++x) p[x] = fn(p[x]);
	}
}

// Brightness level L (0..510): L <= 255 → mul[L][c]; above, a blend towards white.
uint32_t bright(uint32_t c, int L) {
	L = std::clamp(L, 0, 510);
	if (L <= 255) return (c * uint32_t(L) + 127) / 255;
	uint32_t u = uint32_t(L - 255);
	return c * (255 - u) / 255 + u;
}

uint32_t chan(uint32_t p, int c) {
	return (p >> (c * 8)) & 0xff;
}

// (dst, src, dx, dy, rect…) argument block of the copy natives; false if nothing to do.
struct CopyArgs {
	ImageRef dst, src;
	int dx = 0, dy = 0;
	Rect sr;
};
bool copy_args(Engine &e, Args &a, CopyArgs &c, int k = 2) {
	c.dst = e.image_of(a.u(0));
	c.src = e.image_of(a.u(1));
	if (!c.dst || !c.src) return false;
	if (a.u(k) == 0xffff) {
		c.sr = c.src->rect();
		return true;
	}
	c.dx = a.i(k);
	c.dy = a.i(k + 1);
	c.sr = rect_at(a, k + 2, *c.src);
	return true;
}

// Clips (dx,dy,sr) to the destination; returns false if empty.
bool clip_copy(const Image &dst, int &dx, int &dy, Rect &sr) {
	Rect d(dx, dy, dx + sr.w(), dy + sr.h());
	Rect c = d.intersect(dst.rect());
	if (c.empty()) return false;
	sr = Rect(sr.l + c.l - d.l, sr.t + c.t - d.t, sr.l + c.r - d.l, sr.t + c.b - d.t);
	dx = c.l, dy = c.t;
	return true;
}

// Separable box blur averaging over the in-range window (Kirikiri doBoxBlur).
void box_blur(Image &img, const Rect &r, int rx, int ry, bool alpha) {
	if (img.bpp() != 32 || r.empty() || (rx <= 0 && ry <= 0)) return;
	const int w = r.w(), h = r.h();
	std::vector<uint32_t> tmp(size_t(w) * h);
	auto pass = [&](bool horiz, int rad) {
		if (rad <= 0) return;
		int n = horiz ? w : h, lines = horiz ? h : w;
		rad = std::min(rad, n);  // script-supplied; avoid i + rad overflowing
		std::vector<uint32_t> line(n);
		for (int l = 0; l < lines; ++l) {
			auto at = [&](int i) -> uint32_t & { return horiz ? img.row32(r.t + l)[r.l + i] : img.row32(r.t + i)[r.l + l]; };
			for (int i = 0; i < n; ++i) line[i] = at(i);
			uint32_t sum[4] = {0, 0, 0, 0};
			int cnt = 0;
			for (int i = 0; i <= std::min(rad, n - 1); ++i, ++cnt)
				for (int c = 0; c < 4; ++c) sum[c] += chan(line[i], c);
			for (int i = 0; i < n; ++i) {
				uint32_t o = 0;
				for (int c = 0; c < 4; ++c) o |= ((sum[c] + cnt / 2) / cnt) << (c * 8);
				if (!alpha) o = (o & 0xffffff) | (line[i] & 0xff000000u);
				at(i) = o;
				int add = i + rad + 1, sub = i - rad;
				if (add < n) {
					for (int c = 0; c < 4; ++c) sum[c] += chan(line[add], c);
					++cnt;
				}
				if (sub >= 0) {
					for (int c = 0; c < 4; ++c) sum[c] -= chan(line[sub], c);
					--cnt;
				}
			}
		}
	};
	pass(true, rx);
	pass(false, ry);
}

}  // namespace

void register_image_op_natives(Engine &e) {
	auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		e.vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};

	def(0x13b, "image_load_into", [&e](Args &a) -> int32_t {
		ImageRef dst = e.image_of(a.u(0));
		if (!dst) return 1;
		ImageRef src = e.load_image(a.s(1), dst->bpp() == 8);
		if (!src) return 2;
		if (dst->bpp() == 8 && src->bpp() == 8 && a.i(2) == 0 && a.i(3) == 0)
			memcpy(dst->palette(), src->palette(), 256 * 4);
		copy_rect(*dst, a.i(2), a.i(3), *src, src->rect());
		gfx_touch();
		return 0;
	});
	def(0x13e, "image_fill_argb", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return 0;
		fill_rect(*img, rect_at(a, 2, *img), a.u(1));
		gfx_touch();
		return 1;
	});
	def(0x13f, "image_fill_rgb", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img || img->bpp() != 32) return 0;
		uint32_t rgb = ((a.u(1) & 0xff) << 16) | ((a.u(2) & 0xff) << 8) | (a.u(3) & 0xff);
		map_rgb(*img, rect_at(a, 4, *img), [rgb](uint32_t p) { return (p & 0xff000000u) | rgb; });
		gfx_touch();
		return 1;
	});
	def(0x148, "image_copy", [&e](Args &a) -> int32_t {
		CopyArgs c;
		if (!copy_args(e, a, c)) return -1;
		if (c.dst->bpp() == 32 && c.src->bpp() == 32 && c.src->color_key != 0xFFFFFFFF)
			blit(*c.dst, *c.src, c.dx, c.dy, c.sr, 0, 255, false);  // keyed copy
		else
			copy_rect(*c.dst, c.dx, c.dy, *c.src, c.sr);
		gfx_touch();
		return 0;
	});
	def(0x14c, "image_alpha_blend", [&e](Args &a) -> int32_t {
		CopyArgs c;
		if (!copy_args(e, a, c)) return -1;
		if (c.dst->bpp() != 32 || c.src->bpp() != 32) return 2;
		blit(*c.dst, *c.src, c.dx, c.dy, c.sr, kBlendAlpha, 255, false);
		gfx_touch();
		return 0;
	});
	def(0x150, "image_copy_alpha_plane", [&e](Args &a) -> int32_t {
		CopyArgs c;
		if (!copy_args(e, a, c)) return -1;
		if (c.dst->bpp() != 32) return 2;
		if (!clip_copy(*c.dst, c.dx, c.dy, c.sr)) return 0;
		for (int j = 0; j < c.sr.h(); ++j) {
			uint32_t *d = c.dst->row32(c.dy + j) + c.dx;
			for (int i = 0; i < c.sr.w(); ++i) {
				int sx = c.sr.l + i, sy = c.sr.t + j;
				uint32_t v = c.src->bpp() == 8 ? c.src->row8(sy)[sx] : (c.src->row32(sy)[sx] & 0xff);
				d[i] = (d[i] & 0xffffff) | (v << 24);
			}
		}
		c.dst->has_alpha = true;
		gfx_touch();
		return 0;
	});
	def(0x154, "image_copy_rgb", [&e](Args &a) -> int32_t {
		CopyArgs c;
		if (!copy_args(e, a, c)) return -1;
		if (c.dst->bpp() != 32) return 2;
		if (!clip_copy(*c.dst, c.dx, c.dy, c.sr)) return 0;
		for (int j = 0; j < c.sr.h(); ++j) {
			uint32_t *d = c.dst->row32(c.dy + j) + c.dx;
			for (int i = 0; i < c.sr.w(); ++i)
				d[i] = (d[i] & 0xff000000u) | (c.src->pixel(c.sr.l + i, c.sr.t + j) & 0xffffff);
		}
		gfx_touch();
		return 0;
	});
	// N_15a: 32 bpp pixels (8 bpp: the palette); N_17b: the index values of an 8 bpp mask.
	def(0x15a, "image_brightness", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return 0;
		int L = a.i(1);
		map_rgb(*img, rect_at(a, 2, *img), [L](uint32_t p) {
			return (p & 0xff000000u) | bright(p & 0xff, L) | (bright((p >> 8) & 0xff, L) << 8) |
			       (bright((p >> 16) & 0xff, L) << 16);
		});
		gfx_touch();
		return 1;
	});
	def(0x16a, "image_alpha_blend_opa", [&e](Args &a) -> int32_t {
		ImageRef dst = e.image_of(a.u(0)), src = e.image_of(a.u(1));
		if (!dst || !src) return -1;
		int opa = int(a.u(2) & 0xff);
		int dx = 0, dy = 0;
		Rect sr = src->rect();
		if (a.u(3) != 0xffff) {
			dx = a.i(3), dy = a.i(4);
			sr = rect_at(a, 5, *src);
		}
		blit(*dst, *src, dx, dy, sr, src->bpp() == 32 ? kBlendAlphaOver : kBlendCopy, opa, false);
		gfx_touch();
		return 0;
	});
	def(0x175, "image_grayscale", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return -1;
		map_rgb(*img, rect_at(a, 1, *img), [bpp = img->bpp()](uint32_t p) {
			uint32_t y = bpp == 8 ? (chan(p, 2) * 77 + chan(p, 1) * 150 + chan(p, 0) * 29) >> 8
			                      : (chan(p, 0) * 77 + chan(p, 1) * 150 + chan(p, 2) * 29) >> 8;
			return (p & 0xff000000u) | y | (y << 8) | (y << 16);
		});
		gfx_touch();
		return 0;
	});
	def(0x176, "image_color_matrix", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return -1;
		int mul[3] = {std::min(a.i(3), 256), std::min(a.i(2), 256), std::min(a.i(1), 256)};  // B, G, R
		int add[3] = {a.i(6), a.i(5), a.i(4)};
		map_rgb(*img, rect_at(a, 7, *img), [mul, add](uint32_t p) {
			uint32_t o = p & 0xff000000u;
			for (int c = 0; c < 3; ++c) o |= uint32_t(fx::clamp8(add[c] + ((int(chan(p, c)) * mul[c]) >> 7))) << (c * 8);
			return o;
		});
		gfx_touch();
		return 0;
	});
	def(0x177, "image_invert", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return -1;
		int mode = a.i(1);
		uint32_t mask = mode == 0 ? 0x00ffffffu : mode == 1 ? 0xffffffffu : 0xff000000u;
		if (img->bpp() == 8) {
			Rect r = rect_at(a, 2, *img);
			for (int y = r.t; y < r.b; ++y)
				for (int x = r.l; x < r.r; ++x) img->row8(y)[x] ^= 0xff;
		} else {
			map_rgb(*img, rect_at(a, 2, *img), [mask](uint32_t p) { return p ^ mask; });
		}
		gfx_touch();
		return 0;
	});
	def(0x17b, "image8_brightness", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img || img->bpp() != 8) return 0;
		Rect r = rect_at(a, 2, *img);
		for (int y = r.t; y < r.b; ++y)
			for (int x = r.l; x < r.r; ++x) img->row8(y)[x] = uint8_t(bright(img->row8(y)[x], a.i(1)));
		gfx_touch();
		return 1;
	});
	def(0x185, "image_box_blur", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return -1;
		box_blur(*img, rect_at(a, 4, *img), a.i(1), a.i(2), a.b(3));
		gfx_touch();
		return 0;
	});
	def(0x186, "image_flip_v", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return -1;
		Rect r = rect_at(a, 1, *img);
		int bpp = img->bpp() / 8;
		std::vector<uint8_t> tmp(size_t(r.w()) * bpp);
		for (int i = 0; i < r.h() / 2; ++i) {
			uint8_t *p = img->row8(r.t + i) + r.l * bpp, *q = img->row8(r.b - 1 - i) + r.l * bpp;
			memcpy(tmp.data(), p, tmp.size());
			memcpy(p, q, tmp.size());
			memcpy(q, tmp.data(), tmp.size());
		}
		gfx_touch();
		return 0;
	});
	def(0x187, "image_flip_h", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return -1;
		Rect r = rect_at(a, 1, *img);
		for (int y = r.t; y < r.b; ++y) {
			if (img->bpp() == 8)
				std::reverse(img->row8(y) + r.l, img->row8(y) + r.r);
			else
				std::reverse(img->row32(y) + r.l, img->row32(y) + r.r);
		}
		gfx_touch();
		return 0;
	});
	def(0x18c, "image_rule_blend", [&e](Args &a) -> int32_t {
		ImageRef dst = e.image_of(a.u(0)), s1 = e.image_of(a.u(1)), s2 = e.image_of(a.u(2)), rule = e.image_of(a.u(3));
		if (!dst || !s1 || !s2 || !rule || rule->bpp() != 8 || dst->bpp() != 32 || s1->bpp() != 32 || s2->bpp() != 32)
			return -1;
		int opa = int(a.u(4) & 0xff), vague = a.i(5);
		int dx = a.i(7), dy = a.i(8), w = dst->width(), h = dst->height();
		int s1x = 0, s1y = 0, s2x = 0, s2y = 0, rx = 0, ry = 0;
		if (a.u(9) != 0xffff) {
			w = a.i(9), h = a.i(10);
			s1x = a.i(11), s1y = a.i(12), s2x = a.i(13), s2y = a.i(14), rx = a.i(15), ry = a.i(16);
		}
		rule_blend(*dst, *s1, *s2, *rule, opa, vague, dx, dy, w, h, s1x, s1y, s2x, s2y, rx, ry);
		gfx_touch();
		return 0;
	});
}

}  // namespace ore
