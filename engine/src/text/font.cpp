// Glyph fonts (docs/re/40_text.md §9): platform rasterisation, the original's
// cell/baseline conventions, AA quantisation, edge dilation, shadow, gradient
// and the two blend modes.
#include "text/font.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "core/engine.h"
#include "gfx/surface.h"

namespace ore {

namespace {

struct Glyph {
	int w = 0, h = 0;    // bitmap size
	int ox = 0, oy = 0;  // bitmap offset from the top-left of the line cell
	int adv = 0;         // platform advance (proportional faces)
	std::vector<uint8_t> a;
};

// The Chinese patch remaps nine SJIS title glyphs to GBK (GetGlyphOutlineA hook).
uint16_t chs_title_remap(uint16_t c) {
	switch (c) {
	case 0x8148: return 0xa3bf;
	case 0x89b4: return 0xb0b3;
	case 0x82bd: return 0xa4bf;
	case 0x82bf: return 0xa4c1;
	case 0x82c9: return 0xa4cb;
	case 0x9783: return 0xd2ed;
	case 0x82cd: return 0xa4cf;
	case 0x82c8: return 0xa4ca;
	case 0x82a2: return 0xa4a4;
	default: return c;
	}
}

uint8_t quantize(int a, int aa) {
	switch (aa) {
	case 0: return a >= 128 ? 255 : 0;
	case 2: return uint8_t(std::min(((a * 64 + 127) / 255) * 4, 255));
	default: return uint8_t(std::min(((a * 16 + 127) / 255) * 16, 255));
	}
}

// Grayscale dilation of FUN_0044bb80 (octagonal distance, weights r-d+1).
Glyph make_edge(const Glyph &g, int r, int density) {
	Glyph e;
	e.w = g.w + 2 * r;
	e.h = g.h + 2 * r;
	e.ox = g.ox - r;
	e.oy = g.oy - r;
	e.adv = g.adv;
	e.a.assign(size_t(e.w) * e.h, 0);
	if (g.a.empty() || r <= 0) return e;
	struct Tap {
		int dx, dy, wgt;
	};
	std::vector<Tap> taps;
	int sum = 0;
	for (int dy = -r; dy <= r; ++dy)
		for (int dx = -r; dx <= r; ++dx) {
			int a = std::min(std::abs(dx), std::abs(dy)), b = std::max(std::abs(dx), std::abs(dy));
			int a2 = a + a / 2;
			int d = b + (a2 >> 2) + (a2 >> 6) - (b >> 7) - (b >> 5);
			if (d > r) continue;
			taps.push_back({dx, dy, r - d + 1});
			sum += r - d + 1;
		}
	int norm = 0x10000 / std::max(1, sum);
	for (Tap &t : taps) t.wgt = (t.wgt * density * norm) >> 8;
	std::vector<int> acc(e.a.size(), 0);
	for (int y = 0; y < g.h; ++y)
		for (int x = 0; x < g.w; ++x) {
			int s = g.a[size_t(y) * g.w + x];
			if (!s) continue;
			for (const Tap &t : taps) {
				size_t i = size_t(y + r + t.dy) * e.w + (x + r + t.dx);
				acc[i] += (s * t.wgt) >> 16;
			}
		}
	for (size_t i = 0; i < acc.size(); ++i) e.a[i] = uint8_t(std::min(acc[i], 255));
	return e;
}

}  // namespace

void blend_text_pixel(uint32_t &d, uint32_t rgb, int a, bool over) {
	if (a <= 0) return;
	int dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff, da = int(d >> 24);
	int cr = (rgb >> 16) & 0xff, cg = (rgb >> 8) & 0xff, cb = rgb & 0xff;
	int w = a, oa = da;
	if (over) {
		w = da == 0 ? 255 : 255 * a / std::max(1, a + da * (255 - a) / 255);
		oa = 255 - (255 - a) * (255 - da) / 255;
	}
	dr += ((cr - dr) * (w + 1)) >> 8;
	dg += ((cg - dg) * (w + 1)) >> 8;
	db += ((cb - db) * (w + 1)) >> 8;
	d = uint32_t(oa) << 24 | uint32_t(dr & 0xff) << 16 | uint32_t(dg & 0xff) << 8 | uint32_t(db & 0xff);
}

struct Font::Impl {
	std::unordered_map<uint64_t, Glyph> cache;

	static uint64_t key(uint16_t code, bool edge, bool bold, bool italic, int size, int aa) {
		return uint64_t(edge) << 31 | uint64_t(bold) << 30 | uint64_t(italic) << 29 | uint64_t(size & 0x3ff) << 16 |
		       code | uint64_t(aa) << 32;
	}
};

Font::Font(Engine &e, int s, const std::string &f, bool r) : engine(e), rich(r), face(f), size(s), impl(new Impl) {}

Font::~Font() {
	delete impl;
}

void Font::invalidate_cache() {
	impl->cache.clear();
}

void Font::set_size(int px) {
	if (px < 1) px = 1;
	if (px > 1000) px = 1000;
	size = px;
}

void Font::set_face(const std::string &f) {
	if (f == face) return;
	face = f;
	invalidate_cache();
}

void Font::set_style(bool b, bool i) {
	bold = b;
	italic = i;
}

void Font::set_antialias(int mode) {
	antialias = mode < 0 ? 0 : mode > 2 ? 2 : mode;
}

int Font::ascent() const {
	return (size * 86 + 50) / 100;  // MS Gothic tmAscent / em
}

uint16_t Font::next_char(const char *&p, TextEncoding enc) {
	uint8_t c = uint8_t(*p);
	if (!c) return 0;
	if (is_lead_byte(c, enc) && p[1]) {
		uint16_t code = uint16_t(c << 8 | uint8_t(p[1]));
		p += 2;
		return code;
	}
	++p;
	return c;
}

namespace {

// Fixed-pitch faces (MS Gothic/Mincho): full-width = em, half-width = em/2.
// Proportional "Ｐ" faces use the platform advance.
bool fixed_pitch(const std::string &face) {
	return face.find("\x82\x6f") == std::string::npos;
}

const Glyph &get_glyph(Font &f, uint16_t code, bool edge, bool bold) {
	auto &cache = f.impl->cache;
	uint64_t k = Font::Impl::key(code, edge, bold, f.italic, f.size, f.antialias);
	auto it = cache.find(k);
	if (it != cache.end()) return it->second;
	if (cache.size() > 2048) cache.clear();
	Glyph g;
	if (edge) {
		g = make_edge(get_glyph(f, code, false, bold), f.edge_radius, f.edge_density);
		return cache.emplace(k, std::move(g)).first->second;
	}
	Engine &e = f.engine;
	uint16_t c = e.encoding == TextEncoding::Gbk ? chs_title_remap(code) : code;
	uint32_t cp = e.platform.decode_char(c, e.encoding);
	GlyphBitmap gb;
	if (cp > 0x20 && cp != 0x3000 && e.platform.render_glyph(cp, f.face, f.size, bold ? 700 : 400, gb)) {
		g.w = gb.width;
		g.h = gb.height;
		g.ox = gb.left;
		g.oy = f.ascent() - gb.top;
		g.adv = gb.advance;
		g.a.resize(gb.alpha.size());
		for (size_t i = 0; i < gb.alpha.size(); ++i) g.a[i] = quantize(gb.alpha[i], f.antialias);
		if (f.italic && g.w > 0) {
			// shear right by 1/4 of the height above the baseline
			// glyphs can reach above the ascent: size the buffer from the largest shift
			int extra = std::max(0, f.ascent() - g.oy) / 4 + 1;
			Glyph s = g;
			s.w = g.w + extra;
			s.a.assign(size_t(s.w) * s.h, 0);
			for (int y = 0; y < g.h; ++y) {
				int sh = std::min(extra - 1, std::max(0, f.ascent() - (g.oy + y)) / 4);
				for (int x = 0; x < g.w; ++x) s.a[size_t(y) * s.w + x + sh] = g.a[size_t(y) * g.w + x];
			}
			g = std::move(s);
		}
	} else {
		g.adv = cp == 0x3000 ? f.size : f.size / 2;
		if (cp == 0x20 && e.platform.render_glyph(cp, f.face, f.size, 400, gb)) g.adv = gb.advance;
	}
	return cache.emplace(k, std::move(g)).first->second;
}

void put_glyph(Image &dst, int gx, int gy, const Glyph &g, uint32_t rgb, const uint32_t *rows, int cell_y, int nrows,
               bool over, Rect *dirty) {
	if (dst.bpp() != 32 || g.a.empty()) return;
	Rect clip = dst.clip.empty() ? dst.rect() : dst.clip.intersect(dst.rect());
	Rect r = Rect(gx, gy, gx + g.w, gy + g.h).intersect(clip);
	if (r.empty()) return;
	for (int y = r.t; y < r.b; ++y) {
		uint32_t *d = dst.row32(y);
		const uint8_t *s = &g.a[size_t(y - gy) * g.w];
		uint32_t c = rgb;
		if (rows) c = rows[std::min(std::max(y - cell_y, 0), nrows - 1)];
		for (int x = r.l; x < r.r; ++x) blend_text_pixel(d[x], c, s[x - gx], over);
	}
	if (dirty) {
		if (dirty->empty())
			*dirty = r;
		else
			*dirty = Rect(std::min(dirty->l, r.l), std::min(dirty->t, r.t), std::max(dirty->r, r.r),
			              std::max(dirty->b, r.b));
	}
}

uint32_t lerp_rgb(uint32_t a, uint32_t b, int num, int den) {
	if (den <= 0) return a;
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
		out |= uint32_t(ca + (cb - ca) * num / den) << sh;
	}
	return out;
}

}  // namespace

int Font::advance(uint16_t code) {
	int cx;
	if (code == '\n' || code == '\r') return 0;
	if (fixed_pitch(face))
		cx = code > 0xff ? size : size / 2;
	else
		cx = get_glyph(*this, code, false, false).adv;
	if (bold) cx = cx + 1 + cx / 50;
	return cx;
}

int Font::draw_char(Image &dst, int x, int y, uint16_t code, bool over, Rect *dirty) {
	if (effect == 2) {
		const Glyph &eg = get_glyph(*this, code, true, bold);
		put_glyph(dst, x + eg.ox, y + eg.oy, eg, effect_color, nullptr, 0, 1, over, dirty);
	} else if (effect == 1) {
		const Glyph &g = get_glyph(*this, code, false, bold);
		put_glyph(dst, x + g.ox + shadow_dx, y + g.oy + shadow_dy, g, effect_color, nullptr, 0, 1, over, dirty);
	}
	const Glyph &m = get_glyph(*this, code, false, bold);  // the edge lookup may have rehashed
	if (gradient && size > 1) {
		std::vector<uint32_t> rows(static_cast<size_t>(size), 0u);
		for (int i = 0; i < size; ++i) rows[size_t(i)] = lerp_rgb(grad_top, grad_bottom, i, size - 1);
		put_glyph(dst, x + m.ox, y + m.oy, m, color, rows.data(), y, size, over, dirty);
	} else {
		put_glyph(dst, x + m.ox, y + m.oy, m, color, nullptr, 0, 1, over, dirty);
	}
	gfx_touch();
	return advance(code);
}

int Font::draw(Image &dst, int x, int y, const std::string &text, bool over, Rect *dirty) {
	const char *p = text.c_str();
	int x0 = x;
	while (*p) {
		uint16_t c = next_char(p, engine.encoding);
		if (c == '\r') continue;
		if (c == '\n') {
			x = x0;
			y += size + line_spacing;
			continue;
		}
		x += draw_char(dst, x, y, c, over, dirty);
	}
	return x;
}

int Font::measure(const std::string &text, int &w, int &h, int *off_x, int *off_y) {
	w = h = 0;
	int lines = 1, cur = 0;
	const char *p = text.c_str();
	while (*p) {
		uint8_t c = uint8_t(*p);
		if (c == '\n') {
			w = std::max(w, cur);
			cur = 0;
			h += size + line_spacing;
			++lines;
			++p;
			continue;
		}
		if (c == '\r') {
			++p;
			continue;
		}
		cur += advance(next_char(p, engine.encoding));
	}
	w = std::max(w, cur);
	h += size;
	if (italic) w += size / 3;
	if (off_x) *off_x = 0;
	if (off_y) *off_y = 0;
	if (effect == 1) {
		w += std::abs(shadow_dx);
		h += std::abs(shadow_dy);
		if (off_x && shadow_dx < 0) *off_x = shadow_dx;
		if (off_y && shadow_dy < 0) *off_y = shadow_dy;
	} else if (effect == 2) {
		w += edge_radius * 2;
		h += edge_radius * 2;
		if (off_x) *off_x = -edge_radius;
		if (off_y) *off_y = -edge_radius;
	}
	return lines;
}

}  // namespace ore
