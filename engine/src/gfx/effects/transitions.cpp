// Screen transitions created by N_fb (docs/re/30_effects.md §2).
#include "gfx/effects/transitions.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/args.h"
#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/effects/fx_util.h"
#include "gfx/transition.h"
#include "util/log.h"

namespace ore {
namespace {

using fx::icos;
using fx::mix;

// Elapsed ms since the first rendered frame.
int64_t elapsed(const Transition &t, int64_t now) {
	int64_t e = now - t.start;
	return e < 0 ? 0 : e;
}

// The cross-fade progress byte: min(255, t*255/D).
int progress255(int64_t t, int d) {
	return int(std::min<int64_t>(255, t * 255 / d));
}

void copy_all(Image &dst, const Image &src) {
	copy_rect(dst, 0, 0, src, src.rect());
}

// ConstAlphaBlend_SD over whole images.
void cross_blend(Image &dst, const Image &s1, const Image &s2, int a) {
	if (a <= 0) return copy_all(dst, s1);
	if (a >= 255) return copy_all(dst, s2);
	blend_images(dst, s1, s2, a);
}

// ---------------------------------------------------------------- cross / rule

class Cross : public Transition {
public:
	explicit Cross(int d) : d_(std::max(1, d)) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		int a = progress255(elapsed(*this, now), d_);
		cross_blend(out, from, to, a);
		return a >= 255;
	}

private:
	int d_;
};

}  // namespace

void rule_blend(Image &dst, const Image &s1, const Image &s2, const Image &rule, int a, int vague, int dx, int dy,
                int w, int h, int s1x, int s1y, int s2x, int s2y, int rx, int ry) {
	uint8_t tab[256];
	if (vague < 0) vague = 0;
	int phase = ((vague + 256) * a) >> 8;
	for (int i = 0; i < 256; ++i) {
		if (i < phase - vague)
			tab[i] = 255;
		else if (i < phase)
			tab[i] = fx::clamp8(255 + (phase - vague - i) * 255 / vague);
		else
			tab[i] = 0;
	}
	for (int j = 0; j < h; ++j) {
		int y = dy + j, y1 = s1y + j, y2 = s2y + j, yr = ry + j;
		if (y < 0 || y >= dst.height() || y1 < 0 || y1 >= s1.height() || y2 < 0 || y2 >= s2.height() || yr < 0 ||
		    yr >= rule.height())
			continue;
		uint32_t *d = dst.row32(y);
		const uint32_t *p1 = s1.row32(y1), *p2 = s2.row32(y2);
		const uint8_t *r = rule.row8(yr);
		for (int i = 0; i < w; ++i) {
			int x = dx + i, x1 = s1x + i, x2 = s2x + i, xr = rx + i;
			if (x < 0 || x >= dst.width() || x1 < 0 || x1 >= s1.width() || x2 < 0 || x2 >= s2.width() || xr < 0 ||
			    xr >= rule.width())
				continue;
			int v = r[xr];
			if (a <= 0)
				d[x] = p1[x1];
			else if (a >= 255)
				d[x] = p2[x2];
			else if (v >= phase)
				d[x] = p1[x1] & 0xffffff;
			else if (v < phase - vague)
				d[x] = p2[x2] & 0xffffff;
			else
				d[x] = mix(p1[x1], p2[x2], tab[v]);
		}
	}
}

namespace {

class Rule : public Transition {
public:
	Rule(int d, ImageRef rule, int vague) : d_(std::max(1, d)), rule_(std::move(rule)), vague_(vague) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		int a = progress255(elapsed(*this, now), d_);
		if (a <= 0)
			copy_all(out, from);
		else if (a >= 255)
			copy_all(out, to);
		else
			rule_blend(out, from, to, *rule_, a, vague_, 0, 0, out.width(), out.height(), 0, 0, 0, 0, 0, 0);
		return a >= 255;
	}

private:
	int d_;
	ImageRef rule_;
	int vague_;
};

// ---------------------------------------------------------------- scroll

class Scroll : public Transition {
public:
	Scroll(int d, int dir, int mode) : d_(std::max(1, d)), dir_(dir & 3), mode_(mode) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		const int W = out.width(), H = out.height();
		const int max = (dir_ == 0 || dir_ == 2) ? W : H;
		int p = int(std::min<int64_t>(max, elapsed(*this, now) * max / d_));
		int po = 0, pn = 0;  // offsets of old / new along the axis
		bool old_on_top = false;
		int in_pos = (dir_ < 2) ? p - max : max - p;  // new sliding in
		int out_pos = (dir_ < 2) ? p : -p;           // old sliding out
		if (mode_ == 1) {
			po = out_pos, pn = 0, old_on_top = true;
		} else if (mode_ == 2) {
			po = out_pos, pn = in_pos;
		} else {
			po = 0, pn = in_pos;
		}
		bool horiz = dir_ == 0 || dir_ == 2;
		auto draw = [&](const Image &src, int off) {
			copy_rect(out, horiz ? off : 0, horiz ? 0 : off, src, src.rect());
		};
		if (old_on_top) {
			draw(to, pn);
			draw(from, po);
		} else {
			draw(from, po);
			draw(to, pn);
		}
		return p >= max;
	}

private:
	int d_, dir_, mode_;
};

// ---------------------------------------------------------------- wave

class Wave : public Transition {
public:
	Wave(int d, int mode, int amp, int period, uint32_t c0, uint32_t c1)
	    : d_(std::max(2, d)), mode_(mode), A_(amp), P_(period), c0_(c0), c1_(c1) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		const int W = out.width(), H = out.height();
		int64_t tc = std::min<int64_t>(elapsed(*this, now), d_);
		int half = d_ / 2;
		int64_t u = tc < half ? tc : (tc < d_ ? d_ - tc : 0);
		int s = icos(int((u << 7) / half - 128) & 511);
		int amp = int((int64_t(A_) * s) >> 16);
		int step;
		switch (mode_) {
		case 0: step = int((int64_t(P_) * s) >> 16); break;
		case 1: step = int((d_ - tc) * P_ / d_); break;
		case 2: step = int(P_ * tc / d_); break;
		default: step = P_; break;
		}
		int base = mode_ < 3 ? -(H / 2) * step : int(P_ * tc / d_) * A_;
		int a = int(std::min<int64_t>(255, tc * 255 / d_));
		uint32_t fill = (mix(c0_, c1_, fx::w8(a)) & 0xffffff) | (c0_ & 0xff000000u);
		for (int y = 0; y < H; ++y) {
			uint32_t ph = uint32_t(y) * uint32_t(step) + uint32_t(base);
			int off = int((int64_t(icos(int((ph >> 9) - 128) & 511)) * amp) >> 16);
			uint32_t *d = out.row32(y);
			if (off > 0)
				for (int x = 0; x < std::min(off, W); ++x) d[x] = fill;
			if (off < 0)
				for (int x = std::max(W + off, 0); x < W; ++x) d[x] = fill;
			const uint32_t *p1 = from.row32(y), *p2 = to.row32(y);
			for (int x = std::max(off, 0); x < std::min(off + W, W); ++x) d[x] = mix(p1[x - off], p2[x - off], uint32_t(a));
		}
		return a >= 255;
	}

private:
	int d_, mode_, A_, P_;
	uint32_t c0_, c1_;
};

// ---------------------------------------------------------------- mosaic

class Mosaic : public Transition {
public:
	Mosaic(int d, int origin, int max_block, bool aspect)
	    : d_(std::max(1, d)), origin_(origin > 4 || origin < 0 ? 0 : origin), mb_(std::max(1, max_block)), aspect_(aspect) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		const int M = 2 * mb_;
		int p = int(std::min<int64_t>(M, elapsed(*this, now) * M / d_));
		const Image &src = p < mb_ ? from : to;
		int b = p < mb_ ? p : M - p;
		if (b < 2) {
			copy_all(out, src);
			return p >= M;
		}
		const int w = out.width(), h = out.height();
		int bw = b, bh = b;
		if (aspect_) {
			if (w < h)
				bw = std::max(2, w * b / h);
			else
				bh = std::max(2, h * b / w);
		}
		int fx0 = 0, fy0 = 0;
		switch (origin_) {
		case 0: fx0 = ((w - bw) / 2) % bw, fy0 = ((h - bh) / 2) % bh; break;
		case 2: fx0 = w % bw; break;
		case 3: fy0 = h % bh; break;
		case 4: fx0 = w % bw, fy0 = h % bh; break;
		default: break;
		}
		for (int by = 0; by < h;) {
			int hh = (by == 0 && fy0) ? fy0 : bh;
			int ye = std::min(h, by + hh);
			for (int bx = 0; bx < w;) {
				int ww = (bx == 0 && fx0) ? fx0 : bw;
				int xe = std::min(w, bx + ww);
				uint32_t c = src.row32(by)[bx];
				for (int y = by; y < ye; ++y) std::fill(out.row32(y) + bx, out.row32(y) + xe, c);
				bx = xe;
			}
			by = ye;
		}
		return p >= M;
	}

private:
	int d_, origin_, mb_;
	bool aspect_;
};

// ---------------------------------------------------------------- rotaxis

// Scanline fill of a convex quad (TL, TR, BR, BL) with texture coordinates
// interpolated linearly along the edges and spans; nearest sampling.
void fill_quad(Image &dst, const Image &src, const double qx[4], const double qy[4], const double qu[4],
               const double qv[4]) {
	double miny = *std::min_element(qy, qy + 4), maxy = *std::max_element(qy, qy + 4);
	int y0 = std::max(0, int(std::ceil(miny))), y1 = std::min(dst.height(), int(std::ceil(maxy)));
	for (int y = y0; y < y1; ++y) {
		double yc = y;
		double xl = 1e18, xr = -1e18, ul = 0, vl = 0, ur = 0, vr = 0;
		for (int e = 0; e < 4; ++e) {
			int f = (e + 1) & 3;
			double ya = qy[e], yb = qy[f];
			if (ya == yb) continue;
			if (yc < std::min(ya, yb) || yc > std::max(ya, yb)) continue;
			double t = (yc - ya) / (yb - ya);
			double x = qx[e] + (qx[f] - qx[e]) * t;
			double u = qu[e] + (qu[f] - qu[e]) * t, v = qv[e] + (qv[f] - qv[e]) * t;
			if (x < xl) xl = x, ul = u, vl = v;
			if (x > xr) xr = x, ur = u, vr = v;
		}
		if (xr < xl) continue;
		int xa = std::max(0, int(std::ceil(xl))), xb = std::min(dst.width(), int(std::floor(xr)) + 1);
		uint32_t *d = dst.row32(y);
		double span = xr - xl;
		for (int x = xa; x < xb; ++x) {
			double t = span > 0 ? (x - xl) / span : 0;
			int u = int(ul + (ur - ul) * t), v = int(vl + (vr - vl) * t);
			u = std::clamp(u, 0, src.width() - 1);
			v = std::clamp(v, 0, src.height() - 1);
			d[x] = src.row32(v)[u];
		}
	}
}

class RotAxis : public Transition {
public:
	RotAxis(int d, int mode) : d_(std::max(1, d)), mode_(mode & 3) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		const int W = out.width(), H = out.height();
		int u = int(std::min<int64_t>(256, elapsed(*this, now) * 256 / d_));
		int k = u <= 128 ? u : u + 256;
		double c = fx::dcos(k), s = fx::dcos(k - 128);
		const double ux[4] = {-1, 1, 1, -1}, uy[4] = {1, 1, -1, -1};  // TL TR BR BL
		double qx[4], qy[4];
		for (int i = 0; i < 4; ++i) {
			double x = ux[i], y = uy[i], z;
			switch (mode_) {
			case 0: z = -x * s, x = x * c; break;
			case 1: z = x * s, x = x * c; break;
			case 2: z = y * s, y = y * c; break;
			default: z = -y * s, y = y * c; break;
			}
			qx[i] = std::trunc((2 * x / (z + 2) + 1) * (W / 2));
			qy[i] = std::trunc((1 - 2 * y / (z + 2)) * (H / 2));
		}
		const Image &src = u <= 128 ? from : to;
		if (u == 0 || u == 256) {
			copy_all(out, src);
		} else {
			fill_rect(out, out.rect(), 0);
			const double qu[4] = {0, double(W - 1), double(W - 1), 0}, qv[4] = {0, 0, double(H - 1), double(H - 1)};
			fill_quad(out, src, qx, qy, qu, qv);
		}
		return u >= 256;
	}

private:
	int d_, mode_;
};

// ---------------------------------------------------------------- trans (#86 shutter)

class Trans : public Transition {
public:
	Trans(int d, int id, bool rev) : d_(std::max(1, d)), id_(id), rev_(rev) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		int u = int(std::min<int64_t>(256, elapsed(*this, now) * 256 / d_));
		if (rev_) u = 256 - u;
		copy_all(out, from);
		if (u > 255) {
			copy_all(out, to);
		} else {
			if (id_ != 86 && !warned_) {
				warned_ = true;
				log_warn("trans: pattern %d not implemented, using 86", id_);
			}
			int H = to.height(), half = H / 2, d = (half * u) >> 8;
			copy_rect(out, 0, 0, to, Rect(0, half - d, to.width(), half));
			copy_rect(out, 0, H - d, to, Rect(0, half, to.width(), half + d));
		}
		return rev_ ? u <= 0 : u >= 256;
	}

private:
	int d_, id_;
	bool rev_;
	bool warned_ = false;
};

// ---------------------------------------------------------------- quake (type 2)

class Quake : public Transition {
public:
	Quake(int d, int ax, int ay, bool clear, double decay)
	    : d_(d), ax_(ax), ay_(ay), clear_(clear), decay_(decay) {}
	bool render(int64_t now, const Image &from, const Image &, Image &out) override {
		int64_t t = elapsed(*this, now);
		bool fin = false;
		if (d_ != -1 && t >= d_) {
			X_ = Y_ = 0;
			fin = true;
		} else {
			double f = d_ == -1 ? 1.0 : 1.0 - (1.0 - decay_) * double(t) / double(d_ <= 0 ? 1 : d_);
			auto centered = [&](int a) { return int(std::trunc((fx::rnd01() * a - a / 2) * f)); };
			if (ax_ == ay_) {
				X_ = centered(ax_);
				Y_ = centered(ay_);
			} else {
				double sg = sign_ ? -1.0 : 1.0;
				sign_ = !sign_;
				if (ax_ < ay_) {
					X_ = centered(ax_);
					Y_ = int(std::trunc(sg * fx::rnd01() * ay_ * f));
				} else {
					X_ = int(std::trunc(sg * fx::rnd01() * ax_ * f));
					Y_ = centered(ay_);
				}
			}
		}
		const int W = out.width(), H = out.height();
		if (clear_) {
			if (X_ > 0) fill_rect(out, Rect(0, 0, X_, H), 0);
			if (X_ < 0) fill_rect(out, Rect(W + X_, 0, W, H), 0);
			if (Y_ > 0) fill_rect(out, Rect(0, 0, W, Y_), 0);
			if (Y_ < 0) fill_rect(out, Rect(0, H + Y_, W, H), 0);
		}
		copy_rect(out, X_, Y_, from, from.rect());
		return fin;
	}

private:
	int d_, ax_, ay_;
	bool clear_;
	double decay_;
	int X_ = 0, Y_ = 0;
	bool sign_ = false;
};

// ---------------------------------------------------------------- zoomflip

class ZoomFlip : public Transition {
public:
	ZoomFlip(int d, int mode, bool smooth) : d_(std::max(1, d)), mode_(mode), filter_(smooth ? 2 : 0) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		const int W = out.width(), H = out.height();
		int max = (mode_ == 0 || mode_ == 2) ? W : (mode_ == 1 || mode_ == 3) ? H : (mode_ == 6 ? 256 : 512);
		int p = int(std::min<int64_t>(max, elapsed(*this, now) * max / d_));
		auto put = [&](const Image &src, int x, int y, int w, int h, Rect sr, int opa) {
			if (w <= 0 || h <= 0 || sr.empty()) return;
			stretch_blit(out, src, x, y, w, h, sr, 0, filter_, opa, false);
		};
		const Rect full = out.rect();
		switch (mode_) {
		case 0:
			put(from, p, 0, W - p, H, full, 255);
			put(to, 0, 0, p, H, full, 255);
			break;
		case 1:
			put(from, 0, p, W, H - p, full, 255);
			put(to, 0, 0, W, p, full, 255);
			break;
		case 2:
			put(from, 0, 0, W - p, H, full, 255);
			put(to, W - p, 0, p, H, full, 255);
			break;
		case 3:
			put(from, 0, 0, W, H - p, full, 255);
			put(to, 0, H - p, W, p, full, 255);
			break;
		case 4:
		case 5: {
			if (mode_ == 5 && p == last_p_) break;
			int z = p <= 256 ? std::max(1, 256 - p) : p - 256;
			const Image &src = p <= 256 ? from : to;
			int cw = z * W / 256, ch = z * H / 256;
			Rect sr((W - cw) / 2, (H - ch) / 2, (W - cw) / 2 + cw, (H - ch) / 2 + ch);
			int opa = (mode_ == 5 && last_p_ >= 0 && z != 256) ? 8 : 255;
			put(src, 0, 0, W, H, sr, opa);
			break;
		}
		default: {
			copy_all(out, from);
			int cw = (p * W) >> 8, ch = (p * H) >> 8;
			Rect sr((W - cw) / 2, (H - ch) / 2, (W - cw) / 2 + cw, (H - ch) / 2 + ch);
			put(to, 0, 0, W, H, sr, p >= 255 ? 255 : p);
			break;
		}
		}
		last_p_ = p;
		return p >= max;
	}

private:
	int d_, mode_, filter_;
	int last_p_ = -1;
};

// ---------------------------------------------------------------- rotcenter

class RotCenter : public Transition {
public:
	RotCenter(int d, int mode, bool smooth) : d_(std::max(1, d)), mode_(mode) { (void)smooth; }
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		const int W = out.width(), H = out.height();
		int u = int(std::min<int64_t>(512, elapsed(*this, now) * 512 / d_));
		const Image *bgi = &from, *fg = &to;
		int k = u;
		if (mode_ == 1) {
			k = 512 - u;
			bgi = &to, fg = &from;
		}
		int opa = 255;
		if (mode_ == 2) {
			if (u == last_u_) return u >= 512;
			if (last_u_ >= 0) opa = 16;
		} else {
			copy_all(out, *bgi);
		}
		last_u_ = u;
		int sw = (W * k) >> 9, sh = (H * k) >> 9;
		if (sw > 1 && sh > 1) {
			double c = fx::dcos(k), s = fx::dcos(k - 128);
			double kx = double(sw) / W, ky = double(sh) / H;
			double cx = W / 2.0, cy = H / 2.0;
			// (u,v) -> d = (u*kx - sw/2, v*ky - sh/2) rotated about the centre
			double A = c * kx, B = -s * ky, C = s * kx, D = c * ky;
			double tx = cx - c * sw / 2.0 + s * sh / 2.0, ty = cy - s * sw / 2.0 - c * sh / 2.0;
			affine_blit(out, *fg, fg->rect(), A, B, C, D, tx, ty, 0, opa, false);
		}
		if (u >= 512 && mode_ != 1) copy_all(out, to);
		return u >= 512;
	}

private:
	int d_, mode_;
	int last_u_ = -1;
};

// ---------------------------------------------------------------- addsub

class AddSub : public Transition {
public:
	AddSub(int d, int mode) : d_(std::max(1, d)), mode_(mode) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		int a = progress255(elapsed(*this, now), d_);
		if (a >= 255) {
			copy_all(out, to);
			return true;
		}
		const Image &base = (mode_ & 1) ? to : from;
		int k = (mode_ & 1) ? 255 - a : a;
		int sg = mode_ < 2 ? 1 : -1;
		for (int y = 0; y < out.height(); ++y) {
			const uint32_t *s = base.row32(y);
			uint32_t *d = out.row32(y);
			for (int x = 0; x < out.width(); ++x) {
				uint32_t p = s[x];
				d[x] = uint32_t(fx::clamp8(int(p & 0xff) + sg * k)) | (uint32_t(fx::clamp8(int((p >> 8) & 0xff) + sg * k)) << 8) |
				       (uint32_t(fx::clamp8(int((p >> 16) & 0xff) + sg * k)) << 16) | (p & 0xff000000u);
			}
		}
		return false;
	}

private:
	int d_, mode_;
};

// ---------------------------------------------------------------- hblur

class HBlur : public Transition {
public:
	HBlur(int d, int mode, int r) : d_(std::max(1, d)), mode_(mode), R_(std::clamp(r, 1, 127)) {}
	bool render(int64_t now, const Image &from, const Image &to, Image &out) override {
		int64_t t = elapsed(*this, now);
		if (mode_ == 0) {
			int M = 2 * R_ + 1;
			int p = int(std::min<int64_t>(M, t * M / d_));
			if (p <= R_)
				fx::hblur(out, from, p);
			else
				fx::hblur(out, to, M - p);
			return p >= M;
		}
		int a = progress255(t, d_);
		int r = a < 128 ? R_ * a / 127 : R_ * (255 - a) / 127;
		if (r == 0) {
			cross_blend(out, from, to, a);
		} else {
			if (!tmp_ || tmp_->width() != to.width() || tmp_->height() != to.height())
				tmp_ = std::make_shared<Image>(to.width(), to.height(), 32);
			fx::hblur(*tmp_, to, r);
			uint32_t w = fx::w8(a);
			for (int y = 0; y < out.height(); ++y) {
				const uint32_t *s1 = from.row32(y), *b = tmp_->row32(y);
				uint32_t *d = out.row32(y);
				for (int x = 0; x < out.width(); ++x) d[x] = mix(s1[x], b[x], w);
			}
		}
		return a >= 255;
	}

private:
	int d_, mode_, R_;
	ImageRef tmp_;
};

}  // namespace

std::unique_ptr<Transition> make_transition(Engine &e, const std::string &name, Args &a, int k) {
	return make_transition(name, a, k, name == "rule" ? e.image_of(a.u(k + 1)) : nullptr);
}

std::unique_ptr<Transition> make_transition(const std::string &name, Args &a, int k, ImageRef r) {
	int d = a.i(k);
	if (name == "cross") return std::make_unique<Cross>(d);
	if (name == "rule") {
		if (!r || r->bpp() != 8) {
			log_warn("rule transition: rule image missing or not 8 bpp");
			return std::make_unique<Cross>(d);
		}
		return std::make_unique<Rule>(d, r, a.i(k + 2));
	}
	if (name == "scroll") return std::make_unique<Scroll>(d, a.i(k + 1), a.i(k + 2));
	if (name == "wave") return std::make_unique<Wave>(d, a.i(k + 1), a.i(k + 2), a.i(k + 3), a.u(k + 4), a.u(k + 5));
	if (name == "mosaic") return std::make_unique<Mosaic>(d, a.i(k + 1), a.i(k + 2), a.b(k + 3));
	if (name == "rotaxis") return std::make_unique<RotAxis>(d, a.i(k + 1));
	if (name == "trans") return std::make_unique<Trans>(d, a.i(k + 1), a.b(k + 2));
	if (name == "quake")
		return std::make_unique<Quake>(d, a.i(k + 1), a.i(k + 2), a.b(k + 3), fx::atof_or(a.s(k + 4), 0.0));
	if (name == "zoomflip") return std::make_unique<ZoomFlip>(d, a.i(k + 1), a.b(k + 2));
	if (name == "rotcenter") return std::make_unique<RotCenter>(d, a.i(k + 1), a.b(k + 2));
	if (name == "addsub") return std::make_unique<AddSub>(d, a.i(k + 1));
	if (name == "hblur") return std::make_unique<HBlur>(d, a.i(k + 1), a.i(k + 2));
	if (name == "tone") return std::make_unique<Cross>(d);  // unused by the scripts
	return nullptr;
}

void register_transition_natives(Engine &e) {
	e.vm.set_native(0xfb, [&e](Vm &v, uint32_t base) -> int32_t {
		Args a(v, base);
		Surface *s = e.surface_of(a.u(0));
		if (!s) return -1;
		std::string name = a.s(1);
		auto t = make_transition(e, name, a, 4);
		if (!t) {
			log_warn("N_fb: unknown transition '%s'", name.c_str());
			return -2;
		}
		return s->start_transition(std::move(t), int(int8_t(a.u(2) & 0xff)), a.b(3));
	}, "start_transition");
}

}  // namespace ore
