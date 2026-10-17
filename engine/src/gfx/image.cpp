#include "gfx/image.h"

#include <algorithm>
#include <cstring>

#include "res/elg.h"

namespace ore {

Image::Image(int w, int h, int bpp) : w_(std::max(0, w)), h_(std::max(0, h)), bpp_(bpp == 8 ? 8 : 32) {
	pitch_ = ((bpp_ / 8) * w_ + 3) & ~3;
	px_.assign(size_t(pitch_) * h_, 0);
	if (bpp_ == 8) {
		palette_.resize(256);
		for (int i = 0; i < 256; ++i) palette_[i] = uint32_t(i) | (uint32_t(i) << 8) | (uint32_t(i) << 16) | 0xFF000000u;
	}
	clip = rect();
}

std::shared_ptr<Image> Image::from_elg(const std::vector<uint8_t> &data, bool keep8) {
	ElgImage e;
	if (!decode_elg(data.data(), data.size(), e)) return nullptr;
	// The EXE refuses type-2 headers; we decode them anyway (none ship).
	std::shared_ptr<Image> img;
	if (e.bpp == 8 && keep8) {
		img = std::make_shared<Image>(e.width, e.height, 8);
		for (int y = 0; y < e.height; ++y) memcpy(img->row8(y), &e.indices[size_t(y) * e.width], size_t(e.width));
		for (int i = 0; i < 256; ++i) {
			const uint8_t *c = &e.palette[size_t(i) * 4];
			img->palette_[i] = uint32_t(c[0]) | (uint32_t(c[1]) << 8) | (uint32_t(c[2]) << 16) | 0xFF000000u;
		}
	} else {
		img = std::make_shared<Image>(e.width, e.height, 32);
		for (int y = 0; y < e.height; ++y) {
			uint8_t *d = reinterpret_cast<uint8_t *>(img->row32(y));
			memcpy(d, &e.bgra[size_t(y) * e.width * 4], size_t(e.width) * 4);
			if (e.bpp != 32) {
				// RGB decode leaves the alpha byte 0 in the original (0x4c21c0)
				for (int x = 0; x < e.width; ++x) d[x * 4 + 3] = 0;
			}
		}
		img->has_alpha = e.bpp == 32;
	}
	img->offset_x = e.offset_x;
	img->offset_y = e.offset_y;
	return img;
}

uint32_t Image::pixel(int x, int y) const {
	if (x < 0 || y < 0 || x >= w_ || y >= h_) return 0;
	if (bpp_ == 8) return palette_[row8(y)[x]];
	return row32(y)[x];
}

void Image::fill(uint32_t value) {
	if (bpp_ == 8) {
		std::fill(px_.begin(), px_.end(), uint8_t(value));
		return;
	}
	for (int y = 0; y < h_; ++y) std::fill(row32(y), row32(y) + w_, value);
}

std::shared_ptr<Image> Image::clone() const {
	auto c = std::make_shared<Image>(*this);
	return c;
}

void Image::resize(int w, int h) {
	Image n(w, h, bpp_);
	n.palette_ = palette_;
	n.color_key = color_key;
	n.has_alpha = has_alpha;
	n.offset_x = offset_x;
	n.offset_y = offset_y;
	n.tag = tag;
	*this = std::move(n);
}

}  // namespace ore
