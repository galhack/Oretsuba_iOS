// Services the engine needs from the host platform.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/text.h"

namespace ore {

struct GlyphBitmap {
	int width = 0, height = 0;   // bitmap size
	int left = 0, top = 0;       // offset of the bitmap from the pen origin (top = ascent side)
	int advance = 0;             // pen advance in pixels
	std::vector<uint8_t> alpha;  // width*height coverage
};

class Platform {
public:
	virtual ~Platform() = default;

	// Converts one character of game text (1 or 2 bytes) to a Unicode code point.
	virtual uint32_t decode_char(uint16_t code, TextEncoding enc) = 0;
	// Converts a whole game string to UTF-8 (for logs, UI, save titles).
	virtual std::string to_utf8(const std::string &text, TextEncoding enc) = 0;

	// Rasterizes a glyph with the platform font closest to `face`
	// (e.g. "ＭＳ ゴシック"), em size `size` px. `weight` follows LOGFONT (400/700).
	virtual bool render_glyph(uint32_t codepoint, const std::string &face, int size, int weight,
	                          GlyphBitmap &out) = 0;

	// Monotonic milliseconds.
	virtual int64_t now_ms() = 0;
	// Writable directory for saves and settings (exists).
	virtual std::string save_dir() = 0;
	// Local wall-clock time broken down (for save dates).
	struct LocalTime {
		int year, month, day, hour, minute, second, weekday;
	};
	virtual LocalTime local_time() = 0;

	// Modal message box. kind 0 = OK, 1 = Yes/No. The engine polls the
	// answer every frame: -1 pending, else 1 (OK/Yes) or 0 (No).
	virtual void begin_dialog(const std::string &utf8_text, int kind) { (void)utf8_text, (void)kind; }
	virtual int poll_dialog() { return 1; }
	// Text entry for save comments (UTF-8 in/out); -1 pending, 0 cancelled, 1 done.
	virtual void begin_text_input(const std::string &utf8_initial) { (void)utf8_initial; }
	virtual int poll_text_input(std::string &utf8_result) { (void)utf8_result; return 0; }
	// Encodes UTF-8 to the game's encoding (for typed text).
	virtual std::string from_utf8(const std::string &utf8, TextEncoding enc) { (void)enc; return utf8; }
};

}  // namespace ore
