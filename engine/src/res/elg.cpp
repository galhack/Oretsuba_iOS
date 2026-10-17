// Lucifen ELG image decoder. Algorithm follows GARbro's ImageELG.cs (MIT).
#include "res/elg.h"

#include <cstring>

namespace ore {
namespace {

class Reader {
public:
	Reader(const uint8_t *d, size_t n, size_t pos) : d_(d), n_(n), p_(pos) {}
	int u8() { return p_ < n_ ? d_[p_++] : (overrun_ = true, 0xFF); }
	int u16() {
		int lo = u8();
		return lo | (u8() << 8);
	}
	int32_t i32() {
		uint32_t v = u16();
		return int32_t(v | (uint32_t(u16()) << 16));
	}
	void skip(size_t k) { p_ += k; }
	bool overrun() const { return overrun_; }

private:
	const uint8_t *d_;
	size_t n_, p_;
	bool overrun_ = false;
};

// Overlapping forward copy within out; returns false if src is out of range.
bool copy_back(std::vector<uint8_t> &out, long dst, long src, long count) {
	if (src < 0 || dst + count > long(out.size())) return false;
	for (long i = 0; i < count; ++i) out[dst + i] = out[src + i];
	return true;
}

bool unpack_indexed(Reader &r, std::vector<uint8_t> &out) {
	long dst = 0;
	const long size = long(out.size());
	for (;;) {
		int flags = r.u8();
		if (flags == 0xFF || dst >= size || r.overrun()) break;
		long count, pos;
		if ((flags & 0xC0) == 0) {
			count = (flags & 0x20) ? ((flags & 0x1F) << 8) + r.u8() + 33 : (flags & 0x1F) + 1;
			for (long i = 0; i < count && dst < size; ++i) out[dst++] = uint8_t(r.u8());
			continue;
		}
		if ((flags & 0xC0) == 0x40) {
			count = (flags & 0x20) ? ((flags & 0x1F) << 8) + r.u8() + 35 : (flags & 0x1F) + 3;
			uint8_t v = uint8_t(r.u8());
			for (long i = 0; i < count && dst < size; ++i) out[dst++] = v;
			continue;
		}
		if ((flags & 0xC0) == 0x80) {
			if ((flags & 0x30) == 0) {
				count = (flags & 0xF) + 2;
				pos = r.u8() + 2;
			} else if ((flags & 0x30) == 0x10) {
				pos = ((flags & 0xF) << 8) + r.u8() + 3;
				count = r.u8() + 4;
			} else if ((flags & 0x30) == 0x20) {
				pos = ((flags & 0xF) << 8) + r.u8() + 3;
				count = 3;
			} else {
				pos = ((flags & 0xF) << 8) + r.u8() + 3;
				count = 4;
			}
		} else if (flags & 0x20) {
			pos = (flags & 0x1F) + 2;
			count = 2;
		} else {
			pos = (flags & 0x1F) + 1;
			count = 1;
		}
		if (dst + count > size) count = size - dst;
		if (!copy_back(out, dst, dst - pos, count)) return false;
		dst += count;
	}
	return true;
}

// Shared by 24-bit (px=3) and 32-bit colour plane (px=4, alpha set to 0xFF).
bool unpack_rgb(Reader &r, std::vector<uint8_t> &out, int width, int px) {
	long dst = 0;
	const long size = long(out.size());
	auto put_px = [&](int b, int g, int rr) {
		if (dst + px > size) return;
		out[dst++] = uint8_t(b);
		out[dst++] = uint8_t(g);
		out[dst++] = uint8_t(rr);
		if (px == 4) out[dst++] = 0xFF;
	};
	for (;;) {
		int flags = r.u8();
		if (flags == 0xFF || dst >= size || r.overrun()) break;
		long count, pos;
		if ((flags & 0xC0) == 0) {
			count = (flags & 0x20) ? ((flags & 0x1F) << 8) + r.u8() + 33 : (flags & 0x1F) + 1;
			for (long i = 0; i < count; ++i) {
				int b = r.u8(), g = r.u8(), rr = r.u8();
				put_px(b, g, rr);
			}
		} else if ((flags & 0xC0) == 0x40) {
			count = (flags & 0x20) ? ((flags & 0x1F) << 8) + r.u8() + 34 : (flags & 0x1F) + 2;
			int b = r.u8(), g = r.u8(), rr = r.u8();
			for (long i = 0; i < count; ++i) put_px(b, g, rr);
		} else if ((flags & 0xC0) == 0x80) {
			if ((flags & 0x30) == 0) {
				count = (flags & 0xF) + 1;
				pos = r.u8() + 2;
			} else if ((flags & 0x30) == 0x10) {
				pos = ((flags & 0xF) << 8) + r.u8() + 2;
				count = r.u8() + 1;
			} else if ((flags & 0x30) == 0x20) {
				int tmp = r.u8();
				pos = ((((flags & 0xF) << 8) + tmp) << 8) + r.u8() + 4098;
				count = r.u8() + 1;
			} else {
				pos = (flags & 8) ? ((flags & 0x7) << 8) + r.u8() + 10 : (flags & 0x7) + 2;
				count = 1;
			}
			long n = count * px;
			if (dst + n > size) n = size - dst;
			if (!copy_back(out, dst, dst - px * pos, n)) return false;
			dst += n;
		} else {
			long y, x;
			if ((flags & 0x30) == 0) {
				int sel = flags & 0xC;
				if (sel == 0xC) {
					pos = ((flags & 0x3) << 8) + r.u8() + 2058;
					if (!copy_back(out, dst, dst - px * pos, px)) return false;
					dst += px;
					continue;
				}
				y = ((flags & 0x3) << 8) + r.u8() + 16;
				x = sel == 0 ? 0 : sel == 4 ? -1 : 1;
			} else {
				y = (flags & 0xF) + 1;
				int sel = flags & 0x30;
				x = sel == 0x10 ? 0 : sel == 0x20 ? -1 : 1;
			}
			long src = dst + (x - long(width) * y) * px;
			if (dst + px > size || !copy_back(out, dst, src, px)) return false;
			dst += px;
		}
	}
	return true;
}

bool unpack_alpha(Reader &r, std::vector<uint8_t> &out) {
	long dst = 3;
	const long size = long(out.size());
	for (;;) {
		int flags = r.u8();
		if (flags == 0xFF || dst >= size || r.overrun()) break;
		long count, pos;
		if ((flags & 0xC0) == 0) {
			count = (flags & 0x20) ? ((flags & 0x1F) << 8) + r.u8() + 33 : (flags & 0x1F) + 1;
			for (long i = 0; i < count; ++i) {
				int a = r.u8();
				if (dst < size) out[dst] = uint8_t(a);
				dst += 4;
			}
			continue;
		}
		if ((flags & 0xC0) == 0x40) {
			count = (flags & 0x20) ? ((flags & 0x1F) << 8) + r.u8() + 35 : (flags & 0x1F) + 3;
			uint8_t a = uint8_t(r.u8());
			for (long i = 0; i < count; ++i) {
				if (dst < size) out[dst] = a;
				dst += 4;
			}
			continue;
		}
		if ((flags & 0xC0) == 0x80) {
			if ((flags & 0x30) == 0) {
				count = (flags & 0xF) + 2;
				pos = r.u8() + 2;
			} else if ((flags & 0x30) == 0x10) {
				pos = ((flags & 0xF) << 8) + r.u8() + 3;
				count = r.u8() + 4;
			} else if ((flags & 0x30) == 0x20) {
				pos = ((flags & 0xF) << 8) + r.u8() + 3;
				count = 3;
			} else {
				pos = ((flags & 0xF) << 8) + r.u8() + 3;
				count = 4;
			}
		} else if (flags & 0x20) {
			pos = (flags & 0x1F) + 2;
			count = 2;
		} else {
			pos = (flags & 0x1F) + 1;
			count = 1;
		}
		long src = dst - 4 * pos;
		if (src < 0) return false;
		for (long i = 0; i < count && dst < size; ++i) {
			out[dst] = out[src];
			src += 4;
			dst += 4;
		}
	}
	return true;
}

}  // namespace

bool decode_elg(const uint8_t *data, size_t size, ElgImage &out) {
	if (size < 8 || memcmp(data, "ELG", 3) != 0) return false;
	Reader r(data, size, 3);
	int bpp = r.u8();
	int type = bpp, header = 8;
	int x = 0, y = 0;
	if (type == 2) {
		bpp = r.u8();
		header = 13;
	} else if (type == 1) {
		bpp = r.u8();
		x = int16_t(r.u16());
		y = int16_t(r.u16());
		header = 13;
	} else {
		type = 0;
	}
	if (bpp != 8 && bpp != 24 && bpp != 32) return false;
	int w = r.u16(), h = r.u16();
	if (type == 2) {
		x = int16_t(r.u16());
		y = int16_t(r.u16());
	}
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
	out = ElgImage();
	out.width = w;
	out.height = h;
	out.offset_x = x;
	out.offset_y = y;
	out.bpp = bpp;
	out.type = type;
	Reader body(data, size, size_t(header));
	if (type == 2) {
		while (body.u8() != 0 && !body.overrun()) {
			int32_t chunk = body.i32();
			if (chunk < 4) return false;
			body.skip(size_t(chunk - 4));
		}
	}
	const size_t npx = size_t(w) * h;
	out.bgra.assign(npx * 4, 0);
	if (bpp == 8) {
		out.palette.assign(0x400, 0);
		out.indices.assign(npx, 0);
		if (!unpack_indexed(body, out.palette) || !unpack_indexed(body, out.indices)) return false;
		for (size_t i = 0; i < npx; ++i) {
			const uint8_t *c = &out.palette[out.indices[i] * 4];
			out.bgra[i * 4 + 0] = c[0];
			out.bgra[i * 4 + 1] = c[1];
			out.bgra[i * 4 + 2] = c[2];
			out.bgra[i * 4 + 3] = 0xFF;
		}
	} else if (bpp == 24) {
		std::vector<uint8_t> rgb(npx * 3, 0);
		if (!unpack_rgb(body, rgb, w, 3)) return false;
		for (size_t i = 0; i < npx; ++i) {
			out.bgra[i * 4 + 0] = rgb[i * 3 + 0];
			out.bgra[i * 4 + 1] = rgb[i * 3 + 1];
			out.bgra[i * 4 + 2] = rgb[i * 3 + 2];
			out.bgra[i * 4 + 3] = 0xFF;
		}
	} else {
		if (!unpack_rgb(body, out.bgra, w, 4) || !unpack_alpha(body, out.bgra)) return false;
	}
	return !body.overrun();
}

}  // namespace ore
