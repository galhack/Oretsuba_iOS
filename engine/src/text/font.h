// Font objects (docs/re/40_text.md §9, docs/re/10_lifecycle_system.md §7.6):
// glyph fonts (rich, used by text windows) and simple TextOut fonts.
#pragma once

#include <cstdint>
#include <string>

#include "core/object.h"
#include "gfx/image.h"
#include "util/text.h"

namespace ore {

class Engine;

class Font : public Object {
public:
	Font(Engine &e, int size, const std::string &face, bool rich);
	~Font() override;

	Engine &engine;
	bool rich;               // glyph font (N_a4) vs TextOut font (N_a3)
	std::string face;        // game-encoded face name
	int size;                // |lfHeight| in px
	bool bold = false, italic = false;
	int antialias = 1;       // 0 bitmap, 1 GRAY4, 2 GRAY8
	uint32_t color = 0xffffff;     // 0xRRGGBB
	int effect = 0;          // 0 none, 1 shadow, 2 edge
	uint32_t effect_color = 0;
	int shadow_dx = 0, shadow_dy = 0;
	int line_spacing = 0;
	bool gradient = false;
	uint32_t grad_top = 0, grad_bottom = 0;
	int edge_radius = 2, edge_density = 0x400;  // FUN_0044bb80

	// Text in the game encoding. `over` selects the ARGB "over" blend used by
	// text objects (else plain alpha blend keeping destination alpha).
	// Returns the pen x after the last character.
	int draw(Image &dst, int x, int y, const std::string &text, bool over, Rect *dirty = nullptr);
	// Draws one character code (1 or 2 bytes) with its top-left at (x, y) of
	// the line cell; returns the advance.
	int draw_char(Image &dst, int x, int y, uint16_t code, bool over, Rect *dirty = nullptr);
	int advance(uint16_t code);
	// Multi-line measure (lines split at '\n' or "\r\n"); returns line count.
	// With effect extents (edge/shadow); off_x/off_y receive the (negative)
	// offset of the effect relative to the pen.
	int measure(const std::string &text, int &w, int &h, int *off_x = nullptr, int *off_y = nullptr);
	void invalidate_cache();

	// Setters that keep the glyph cache consistent.
	void set_size(int px);
	void set_face(const std::string &f);
	void set_style(bool b, bool i);
	void set_antialias(int mode);
	// Baseline offset from the top of the line cell (tmAscent).
	int ascent() const;

	// Reads one character code (DBCS pair or single byte) and advances p.
	static uint16_t next_char(const char *&p, TextEncoding enc);

	struct Impl;
	Impl *impl = nullptr;
};

// Blends one pixel: normal (destination alpha kept) or ARGB "over" (§9.3).
void blend_text_pixel(uint32_t &d, uint32_t rgb, int a, bool over);

}  // namespace ore
