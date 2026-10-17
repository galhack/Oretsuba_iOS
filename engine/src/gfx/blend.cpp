#include "gfx/blend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util/log.h"

namespace ore {
namespace {

inline uint32_t alpha_mix(uint32_t d, uint32_t s, uint32_t a) {
	// TVP packed blend: (((s - d) * a) >> 8) + d per channel pair, alpha := 0
	uint32_t d1 = d & 0xff00ff, s1 = s & 0xff00ff;
	uint32_t rb = ((((s1 - d1) * a) >> 8) + d1) & 0xff00ff;
	uint32_t d2 = d & 0x00ff00, s2 = s & 0x00ff00;
	uint32_t g = ((((s2 - d2) * a) >> 8) + d2) & 0x00ff00;
	return rb | g;
}

inline uint8_t sat(int v) {
	return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

// One destination pixel for a 32 bpp source.
inline uint32_t blend_px(uint32_t d, uint32_t s, int mode, int opa, bool hda) {
	uint32_t keep = hda ? (d & 0xff000000u) : 0;
	switch (mode) {
	case kBlendCopy:
		if (opa >= 255) return hda ? (s & 0x00ffffffu) | keep : s;
		return alpha_mix(d, s, uint32_t(opa)) | keep;
	case kBlendAlpha: {
		uint32_t a = s >> 24;
		if (opa < 255) a = (a * uint32_t(opa)) >> 8;
		return alpha_mix(d, s, a) | keep;
	}
	case kBlendAlphaOver: {
		// N_16a / original method 3 builds reusable ARGB menu panels. Unlike
		// screen blending (method 2), it must retain the resulting coverage.
		uint32_t sa = (s >> 24) * uint32_t(opa) / 255;
		if (!sa) return d;
		uint32_t da = d >> 24;
		uint32_t coverage = sa * 255 + da * (255 - sa);
		uint32_t out = hda ? keep : ((coverage + 127) / 255) << 24;
		for (int shift = 0; shift < 24; shift += 8) {
			uint32_t sc = (s >> shift) & 255, dc = (d >> shift) & 255;
			uint32_t value = (sc * sa * 255 + dc * da * (255 - sa) + coverage / 2) / coverage;
			out |= value << shift;
		}
		return out;
	}
	default: {
		static bool warned[32];
		if (mode >= 0 && mode < 32 && !warned[mode]) {
			warned[mode] = true;
			log_warn("blend: mode %d not implemented, using alpha", mode);
		}
		uint32_t a = s >> 24;
		if (opa < 255) a = (a * uint32_t(opa)) >> 8;
		return alpha_mix(d, s, a) | keep;
	}
	}
}

// Clips a source rect drawn at (x,y) to the destination clip; returns false
// if nothing remains. Adjusts x, y and src.
bool clip_blit(Rect &src, int &x, int &y, const Image &s, const Rect &dclip) {
	src = src.intersect(s.rect());
	if (src.empty()) return false;
	int dx0 = x, dy0 = y;
	Rect d(dx0, dy0, dx0 + src.w(), dy0 + src.h());
	Rect c = d.intersect(dclip);
	if (c.empty()) return false;
	src.l += c.l - d.l;
	src.t += c.t - d.t;
	src.r = src.l + c.w();
	src.b = src.t + c.h();
	x = c.l;
	y = c.t;
	return true;
}

}  // namespace

void blit(Image &dst, const Image &src, int x, int y, Rect sr, int mode, int opa, bool hda, const Rect *clip) {
	if (opa <= 0 || mode == 13) return;
	if (opa > 255) opa = 255;
	Rect dclip = clip ? clip->intersect(dst.rect()) : dst.rect();
	if (!clip_blit(sr, x, y, src, dclip)) return;
	const int w = sr.w(), h = sr.h();
	if (dst.bpp() == 8) {
		if (src.bpp() != 8) return;
		for (int j = 0; j < h; ++j) {
			const uint8_t *s = src.row8(sr.t + j) + sr.l;
			uint8_t *d = dst.row8(y + j) + x;
			for (int i = 0; i < w; ++i)
				if (src.color_key == 0xFFFFFFFF || s[i] != uint8_t(src.color_key)) d[i] = s[i];
		}
		return;
	}
	if (src.bpp() == 8) {
		// opaque copy through the palette; key index skipped; mode/opacity ignored
		const uint32_t *pal = src.palette();
		for (int j = 0; j < h; ++j) {
			const uint8_t *s = src.row8(sr.t + j) + sr.l;
			uint32_t *d = dst.row32(y + j) + x;
			for (int i = 0; i < w; ++i) {
				if (src.color_key != 0xFFFFFFFF && s[i] == uint8_t(src.color_key)) continue;
				d[i] = hda ? (pal[s[i]] & 0xffffff) | (d[i] & 0xff000000u) : pal[s[i]];
			}
		}
		return;
	}
	const bool keyed = src.color_key != 0xFFFFFFFF;
	if (!keyed && mode == kBlendCopy && opa == 255 && !hda) {
		for (int j = 0; j < h; ++j) memcpy(dst.row32(y + j) + x, src.row32(sr.t + j) + sr.l, size_t(w) * 4);
		return;
	}
	for (int j = 0; j < h; ++j) {
		const uint32_t *s = src.row32(sr.t + j) + sr.l;
		uint32_t *d = dst.row32(y + j) + x;
		for (int i = 0; i < w; ++i) {
			if (keyed && s[i] == src.color_key) continue;
			d[i] = blend_px(d[i], s[i], mode, opa, hda);
		}
	}
}

void stretch_blit(Image &dst, const Image &src, int x, int y, int dw, int dh, Rect sr, int mode, int filter, int opa,
                  bool hda, const Rect *clip) {
	if (opa <= 0 || dw == 0 || dh == 0) return;
	sr = sr.intersect(src.rect());
	if (sr.empty()) return;
	if (dw == sr.w() && dh == sr.h()) {
		blit(dst, src, x, y, sr, mode, opa, hda, clip);
		return;
	}
	bool flip_x = dw < 0, flip_y = dh < 0;
	if (flip_x) {
		dw = -dw;
		x -= dw;
	}
	if (flip_y) {
		dh = -dh;
		y -= dh;
	}
	Rect dclip = clip ? clip->intersect(dst.rect()) : dst.rect();
	Rect d = Rect(x, y, x + dw, y + dh).intersect(dclip);
	if (d.empty()) return;
	const int64_t stepx = (int64_t(sr.w()) << 16) / dw, stepy = (int64_t(sr.h()) << 16) / dh;
	const bool bilinear = (filter & 0xf) >= 2 && opa >= 255 && mode == kBlendCopy && src.bpp() == 32;
	for (int py = d.t; py < d.b; ++py) {
		int ry = flip_y ? (dh - 1 - (py - y)) : (py - y);
		int64_t fy = ry * stepy + (bilinear ? (stepy >> 1) - 0x8000 : 0);
		int sy = sr.t + int(std::max<int64_t>(0, fy) >> 16);
		if (sy >= sr.b) sy = sr.b - 1;
		uint32_t *drow = dst.row32(py);
		for (int px = d.l; px < d.r; ++px) {
			int rx = flip_x ? (dw - 1 - (px - x)) : (px - x);
			int64_t fx = rx * stepx + (bilinear ? (stepx >> 1) - 0x8000 : 0);
			int sx = sr.l + int(std::max<int64_t>(0, fx) >> 16);
			if (sx >= sr.r) sx = sr.r - 1;
			uint32_t s;
			if (bilinear) {
				int sx1 = std::min(sx + 1, sr.r - 1), sy1 = std::min(sy + 1, sr.b - 1);
				uint32_t ax = uint32_t((std::max<int64_t>(0, fx) >> 8) & 0xff);
				uint32_t ay = uint32_t((std::max<int64_t>(0, fy) >> 8) & 0xff);
				uint32_t p00 = src.row32(sy)[sx], p01 = src.row32(sy)[sx1];
				uint32_t p10 = src.row32(sy1)[sx], p11 = src.row32(sy1)[sx1];
				uint32_t top = alpha_mix(p00, p01, ax) | ((((p00 >> 24) * (256 - ax) + (p01 >> 24) * ax) >> 8) << 24);
				uint32_t bot = alpha_mix(p10, p11, ax) | ((((p10 >> 24) * (256 - ax) + (p11 >> 24) * ax) >> 8) << 24);
				s = alpha_mix(top, bot, ay) | ((((top >> 24) * (256 - ay) + (bot >> 24) * ay) >> 8) << 24);
			} else {
				s = src.pixel(sx, sy);
			}
			if (src.color_key != 0xFFFFFFFF && src.bpp() == 32 && src.row32(sy)[sx] == src.color_key) continue;
			if (src.bpp() == 8) {
				if (src.color_key != 0xFFFFFFFF && src.row8(sy)[sx] == uint8_t(src.color_key)) continue;
				drow[px] = s;
				continue;
			}
			drow[px] = blend_px(drow[px], s, mode, opa >= 255 ? 255 : opa, hda);
		}
	}
}

void affine_blit(Image &dst, const Image &src, Rect sr, double a, double b, double c, double d, double tx, double ty,
                 int mode, int opa, bool hda, const Rect *clip) {
	sr = sr.intersect(src.rect());
	if (sr.empty() || opa <= 0) return;
	double det = a * d - b * c;
	if (std::fabs(det) < 1e-12) return;
	// forward: X = a*u + b*v + tx ; Y = c*u + d*v + ty   (u,v relative to sr.l, sr.t)
	double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
	// destination bounding box
	double xs[4], ys[4];
	double us[4] = {0, double(sr.w()), 0, double(sr.w())}, vs[4] = {0, 0, double(sr.h()), double(sr.h())};
	for (int k = 0; k < 4; ++k) {
		xs[k] = a * us[k] + b * vs[k] + tx;
		ys[k] = c * us[k] + d * vs[k] + ty;
	}
	Rect box(int(std::floor(*std::min_element(xs, xs + 4))), int(std::floor(*std::min_element(ys, ys + 4))),
	         int(std::ceil(*std::max_element(xs, xs + 4))), int(std::ceil(*std::max_element(ys, ys + 4))));
	Rect dclip = clip ? clip->intersect(dst.rect()) : dst.rect();
	box = box.intersect(dclip);
	for (int py = box.t; py < box.b; ++py) {
		uint32_t *drow = dst.row32(py);
		for (int px = box.l; px < box.r; ++px) {
			double X = px + 0.5 - tx, Y = py + 0.5 - ty;
			double u = ia * X + ib * Y, v = ic * X + id * Y;
			if (u < 0 || v < 0 || u >= sr.w() || v >= sr.h()) continue;
			int sx = sr.l + int(u), sy = sr.t + int(v);
			uint32_t s = src.pixel(sx, sy);
			if (src.bpp() == 8) {
				if (src.color_key != 0xFFFFFFFF && src.row8(sy)[sx] == uint8_t(src.color_key)) continue;
				drow[px] = s;
				continue;
			}
			if (src.color_key != 0xFFFFFFFF && s == src.color_key) continue;
			drow[px] = blend_px(drow[px], s, mode, opa >= 255 ? 255 : opa, hda);
		}
	}
}

void fill_rect(Image &dst, Rect r, uint32_t value) {
	r = r.intersect(dst.rect());
	if (r.empty()) return;
	for (int y = r.t; y < r.b; ++y) {
		if (dst.bpp() == 8)
			memset(dst.row8(y) + r.l, int(value & 0xff), size_t(r.w()));
		else
			std::fill(dst.row32(y) + r.l, dst.row32(y) + r.r, value);
	}
}

void color_fill(Image &dst, Rect r, uint32_t rgb, int opa, int mode) {
	r = r.intersect(dst.rect());
	if (r.empty() || opa <= 0 || dst.bpp() != 32) return;
	rgb &= 0xffffff;
	if (mode == 0) {
		if (opa >= 255) {
			fill_rect(dst, r, rgb);
			return;
		}
		for (int y = r.t; y < r.b; ++y) {
			uint32_t *d = dst.row32(y);
			for (int x = r.l; x < r.r; ++x) d[x] = alpha_mix(d[x], rgb, uint32_t(opa));
		}
		return;
	}
	if (mode == 1) return;
	int cb = int(rgb & 0xff) * opa / 255, cg = int((rgb >> 8) & 0xff) * opa / 255, cr = int((rgb >> 16) & 0xff) * opa / 255;
	int sign = mode == 3 ? -1 : 1;
	for (int y = r.t; y < r.b; ++y) {
		uint32_t *d = dst.row32(y);
		for (int x = r.l; x < r.r; ++x) {
			uint32_t p = d[x];
			d[x] = uint32_t(sat(int(p & 0xff) + sign * cb)) | (uint32_t(sat(int((p >> 8) & 0xff) + sign * cg)) << 8) |
			       (uint32_t(sat(int((p >> 16) & 0xff) + sign * cr)) << 16) | (p & 0xff000000u);
		}
	}
}

void copy_rect(Image &dst, int x, int y, const Image &src, Rect sr) {
	Rect dclip = dst.rect();
	if (!clip_blit(sr, x, y, src, dclip)) return;
	if (dst.bpp() == src.bpp()) {
		int bpp = dst.bpp() / 8;
		for (int j = 0; j < sr.h(); ++j)
			memcpy(dst.row8(y + j) + x * bpp, src.row8(sr.t + j) + sr.l * bpp, size_t(sr.w()) * bpp);
		return;
	}
	if (dst.bpp() == 32) {
		for (int j = 0; j < sr.h(); ++j)
			for (int i = 0; i < sr.w(); ++i) dst.row32(y + j)[x + i] = src.pixel(sr.l + i, sr.t + j);
	}
}

void blend_images(Image &dst, const Image &a, const Image &b, int opa_b) {
	int w = std::min({dst.width(), a.width(), b.width()}), h = std::min({dst.height(), a.height(), b.height()});
	if (opa_b < 0) opa_b = 0;
	if (opa_b > 255) opa_b = 255;
	for (int y = 0; y < h; ++y) {
		const uint32_t *pa = a.row32(y), *pb = b.row32(y);
		uint32_t *d = dst.row32(y);
		for (int x = 0; x < w; ++x) d[x] = alpha_mix(pa[x], pb[x], uint32_t(opa_b));
	}
}

}  // namespace ore
