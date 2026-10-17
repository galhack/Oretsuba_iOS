// Shared helpers of the effect code (docs/re/30_effects.md §1.3).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/args.h"
#include "gfx/image.h"
#include "gfx/surface.h"

namespace ore {

class Engine;

namespace fx {

// icos[i] = trunc(cos(i·π/256)·65536), i = 0..511; icos((k-128)&511) = sin.
int icos(int i);
// dcos[i] = cos(i·π/256) (512 per turn).
double dcos(int i);

// Easing of §1.3 applied to elapsed time t of duration d (returns ms, truncated).
int64_t ease_ms(int64_t t, int64_t d, double e);
double ease_p(double p, double e);
double atof_or(const char *s, double def);

// Uniform RNG (the original uses Knuth's subtractive generator; sequences need not match).
uint32_t rnd();
double rnd01();

// TVP packed blend of s over d with weight a (0..256), alpha := 0.
inline uint32_t mix(uint32_t d, uint32_t s, uint32_t a) {
	uint32_t d1 = d & 0xff00ff, s1 = s & 0xff00ff;
	uint32_t rb = ((((s1 - d1) * a) >> 8) + d1) & 0xff00ff;
	uint32_t d2 = d & 0x00ff00, s2 = s & 0x00ff00;
	uint32_t g = ((((s2 - d2) * a) >> 8) + d2) & 0x00ff00;
	return rb | g;
}
// ORE's own weight table: w[a] = a ? a+1 : 0.
inline uint32_t w8(int a) {
	return a ? uint32_t(a) + 1 : 0;
}
inline uint8_t clamp8(int v) {
	return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

// Horizontal box blur of radius r (width 2r+1, edges repeated, all 4 channels) (FUN_0046a420).
void hblur(Image &dst, const Image &src, int r);

// The compositor that currently shows `bg` (pages swap their compositor contents at
// transition ends; effects follow the background like the original's compositor objects).
// Matches by background image, else by bg_cache image; falls back to hint's compositor.
Compositor *find_comp(Engine &e, Surface *hint, const ImageRef &bg, const ImageRef &cache = nullptr);

}  // namespace fx
}  // namespace ore
