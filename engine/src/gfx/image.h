// Images as the original engine keeps them (docs/re/20_graphics_core.md §2.1):
// 32 bpp BGRA (straight alpha) or 8 bpp + palette, optional colour key,
// clip rect, the ELG placement offset and an owner tag.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ore {

struct Rect {
	int l = 0, t = 0, r = 0, b = 0;
	Rect() = default;
	Rect(int l_, int t_, int r_, int b_) : l(l_), t(t_), r(r_), b(b_) {}
	int w() const { return r - l; }
	int h() const { return b - t; }
	bool empty() const { return r <= l || b <= t; }
	Rect intersect(const Rect &o) const {
		Rect x(std::max(l, o.l), std::max(t, o.t), std::min(r, o.r), std::min(b, o.b));
		if (x.empty()) return Rect();
		return x;
	}
	bool contains(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
	bool operator==(const Rect &o) const { return l == o.l && t == o.t && r == o.r && b == o.b; }
};

class Image {
public:
	Image(int w, int h, int bpp);  // bpp 8 or 32; pixels zeroed
	static std::shared_ptr<Image> from_elg(const std::vector<uint8_t> &data, bool keep8);

	int width() const { return w_; }
	int height() const { return h_; }
	int bpp() const { return bpp_; }
	int pitch() const { return pitch_; }
	uint8_t *bits() { return px_.data(); }
	const uint8_t *bits() const { return px_.data(); }
	uint32_t *row32(int y) { return reinterpret_cast<uint32_t *>(px_.data() + size_t(y) * pitch_); }
	const uint32_t *row32(int y) const { return reinterpret_cast<const uint32_t *>(px_.data() + size_t(y) * pitch_); }
	uint8_t *row8(int y) { return px_.data() + size_t(y) * pitch_; }
	const uint8_t *row8(int y) const { return px_.data() + size_t(y) * pitch_; }
	Rect rect() const { return Rect(0, 0, w_, h_); }

	uint32_t *palette() { return palette_.data(); }  // 256 BGRA entries (8 bpp)
	const uint32_t *palette() const { return palette_.data(); }

	// 0xFFFFFFFF = none. 32 bpp: compared with the whole pixel incl. alpha.
	uint32_t color_key = 0xFFFFFFFF;
	bool has_alpha = false;
	int offset_x = 0, offset_y = 0;  // ELG type-1 placement offset
	int tag = 0;
	Rect clip;

	uint32_t pixel(int x, int y) const;  // BGRA (palette-expanded for 8 bpp)
	void fill(uint32_t value);           // raw u32 / index store
	std::shared_ptr<Image> clone() const;
	void resize(int w, int h);

private:
	int w_, h_, bpp_, pitch_;
	std::vector<uint8_t> px_;
	std::vector<uint32_t> palette_;
};

using ImageRef = std::shared_ptr<Image>;

}  // namespace ore
