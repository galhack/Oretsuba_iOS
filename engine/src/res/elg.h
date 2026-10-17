// Lucifen ELG image decoder.
#pragma once

#include <cstdint>
#include <vector>

namespace ore {

struct ElgImage {
	int width = 0;
	int height = 0;
	int offset_x = 0;  // placement offset stored in type-1/2 headers
	int offset_y = 0;
	int bpp = 0;       // 8, 24 or 32 in the source file
	int type = 0;      // header type: 0 plain, 1 with offset, 2 with offset + extra chunks
	// Always expanded to premultiplied-free BGRA, row-major, width*height*4.
	std::vector<uint8_t> bgra;
	// For 8-bit images the palette (BGRA) and index plane are kept as well.
	std::vector<uint8_t> palette;
	std::vector<uint8_t> indices;
};

bool decode_elg(const uint8_t *data, size_t size, ElgImage &out);

}  // namespace ore
