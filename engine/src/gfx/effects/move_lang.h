// The move language of sp_move / bg_move (FUN_00401bd0, docs/re/30_effects.md §5.6).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ore {
namespace fx {

// One compiled 32-byte step. op >= 0: a move (op = dw); op < 0: a command.
struct MoveStep {
	enum { kLinear = -1, kEase = -2, kFadeIn = -3, kFadeOut = -4, kFront = -5, kBehind = -6, kLoop = -7, kDuration = -8 };
	int op = 0;
	int dw = 0, dh = 0;      // bounce amplitudes ($ % [ ] ^ _)
	int xoff = 0, yoff = 0;  // bounce start offsets ($ [ / % ^)
	int dx = 0, dy = 0;      // displacement of the step
	int value = 0;           // duration (kDuration)
	double ease = 0;         // exponent (kEase)
};

struct MoveContext {
	bool sprite = false;
	int W = 0, H = 0;          // page size
	int iw = 0, ih = 0;        // sprite source size / background image size
	int home_x = 0, home_y = 0;  // sprite: image placement offset; bg: 0,0
	int cur_x = 0, cur_y = 0;  // current position (sprite pos / bg offset)
};

struct MoveProgram {
	std::vector<MoveStep> steps;
	int final_x = 0, final_y = 0;
	int final_opacity = -1;  // -1 = unchanged
	int order = 0;           // 1 = to front, 2 = behind
};

// Returns 0 on success (codes as the original: 4 = bad character, 5 = empty).
int parse_moves(const char *s, const MoveContext &c, MoveProgram &out);

// Displacement of a move step at local time t of duration d (smooth = cosine curve,
// else linear) (FUN_00403590 / FUN_00403db0).
void move_offset(const MoveStep &st, int64_t t, int64_t d, bool smooth, int &ox, int &oy);

}  // namespace fx
}  // namespace ore
