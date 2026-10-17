#include "gfx/effects/move_lang.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "gfx/effects/fx_util.h"

namespace ore {
namespace fx {
namespace {

bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

// FUN_00402920: unsigned decimal at *p (advances p); false if no digit.
bool parse_int(const char *&p, int &v) {
	if (!is_digit(*p)) return false;
	v = 0;
	while (is_digit(*p)) v = v * 10 + (*p++ - '0');
	return true;
}

// Bounce displacement for b in 0..2·amp (triangle wave starting at offset `off`).
int bounce(int b, int amp, int off) {
	if (b == 0) return 0;
	if (off != 0) {
		int m = -2 * off;
		if (b <= m) return b <= -off ? -b : b + 2 * off;
		int b2 = b - m, lim = amp + off;
		return b2 <= lim ? b2 : 2 * lim - b2;
	}
	return b <= amp ? b : 2 * amp - b;
}

}  // namespace

int parse_moves(const char *s, const MoveContext &c, MoveProgram &out) {
	out = MoveProgram();
	if (!s) return 1;
	int curx = c.cur_x, cury = c.cur_y;
	const char *p = s;
	char al = *p ? *p++ : 0;
	while (al) {
		MoveStep st;
		switch (al) {
		case ' ':
			while ((al = *p++) == ' ') {}
			continue;
		case 'a': {
			bool neg = false;
			char ch = *p++;
			if (ch == '-') neg = true;
			if (ch == '-' || ch == '+') ch = *p++;
			if (is_digit(ch)) {
				std::string num(1, ch);
				while (is_digit(*p)) num += *p++;
				if (*p == '.') {
					num += *p++;
					while (is_digit(*p)) num += *p++;
				}
				st.op = MoveStep::kEase;
				st.ease = std::atof(num.c_str());
				if (neg) st.ease = -st.ease;
			} else {
				st.op = MoveStep::kLinear;
				--p;
			}
			out.steps.push_back(st);
			al = *p ? *p++ : 0;
			continue;
		}
		case 'i':
		case 'o':
		case 'f':
		case 'b':
		case 'L':
			st.op = al == 'i' ? MoveStep::kFadeIn
			        : al == 'o' ? MoveStep::kFadeOut
			        : al == 'f' ? MoveStep::kFront
			        : al == 'b' ? MoveStep::kBehind
			                    : MoveStep::kLoop;
			if (al == 'i') out.final_opacity = 255;
			if (al == 'o') out.final_opacity = 0;
			if (al == 'f') out.order = 1;
			if (al == 'b') out.order = 2;
			out.steps.push_back(st);
			al = *p ? *p++ : 0;
			continue;
		default:
			break;
		}
		if (std::strchr("!$%+-<>[]^_cdlru", al)) {
			int dx = 0, dy = 0;
			for (;;) {
				int n;
				if (al == '!') {
					char ch = *p++;
					if (ch == 'l') {
						if (c.sprite) dx = -(curx + c.iw), dy = c.home_y - cury;
						else dx = -(curx + c.W);
					} else if (ch == 'r') {
						if (c.sprite) dx = c.W - curx, dy = c.home_y - cury;
						else dx = c.iw - curx;
					} else if (ch == 'c') {
						dx = c.home_x - curx, dy = c.H - cury;
					} else if (ch == 'u') {
						dy = -(cury + c.H);
					} else if (ch == 'd') {
						dy = c.ih - cury;
					} else {
						dy = c.H - cury;
						--p;
					}
				} else if (al == 'l') {
					if (c.sprite) dx = c.home_x - (c.W >> 2) - curx, dy = c.home_y - cury;
					else dx = -curx;
				} else if (al == 'r') {
					if (c.sprite) dx = (c.W >> 2) - curx + c.home_x, dy = c.home_y - cury;
					else dx = c.iw - curx - c.W;
				} else if (al == 'c') {
					if (c.sprite) dx = c.home_x - curx, dy = c.home_y - cury;
					else dx = ((c.iw - c.W) >> 1) - curx, dy = ((c.ih - c.H) >> 1) - cury;
				} else if (al == 'u') {
					dy = -cury;
				} else if (al == 'd') {
					dy = c.ih - cury - c.H;
				} else if (al == '<') {
					dx -= parse_int(p, n) ? 25 * n : 25;
				} else if (al == '>') {
					dx += parse_int(p, n) ? 25 * n : 25;
				} else if (al == '-') {
					dy -= parse_int(p, n) ? 25 * n : 25;
				} else if (al == '+') {
					dy += parse_int(p, n) ? 25 * n : 25;
				} else if (al == '$') {
					if (!parse_int(p, n)) n = 1;
					st.dw += 50 * n, st.xoff -= 25 * n;
				} else if (al == '%') {
					if (!parse_int(p, n)) n = 1;
					st.dh += 50 * n, st.yoff -= 25 * n;
				} else if (al == '[') {
					if (!parse_int(p, n)) n = 1;
					st.dw += 50 * n, st.xoff -= 50 * n;
				} else if (al == ']') {
					if (!parse_int(p, n)) n = 1;
					st.dw += 50 * n;
				} else if (al == '^') {
					if (!parse_int(p, n)) n = 1;
					st.dh += 50 * n, st.yoff -= 50 * n;
				} else if (al == '_') {
					if (!parse_int(p, n)) n = 1;
					st.dh += 50 * n;
				} else {
					break;
				}
				al = *p ? *p++ : 0;
			}
			curx += dx;
			cury += dy;
			st.op = st.dw;
			st.dx = dx;
			st.dy = dy;
			out.steps.push_back(st);
			continue;  // `al` is the character that ended the block
		}
		// anything else must be a duration
		--p;
		int n;
		if (!parse_int(p, n)) return out.steps.empty() ? 4 : 2;
		st.op = MoveStep::kDuration;
		st.value = n;
		out.steps.push_back(st);
		al = *p ? *p++ : 0;
	}
	if (out.steps.empty()) return 5;
	out.final_x = curx;
	out.final_y = cury;
	return 0;
}

void move_offset(const MoveStep &st, int64_t t, int64_t d, bool smooth, int &ox, int &oy) {
	if (d <= 0) d = 1;
	int b, bh;
	if (smooth) {
		uint32_t e = uint32_t((uint64_t(t) << 8) / uint64_t(d));
		int f = icos(int((0x100 - e) & 0x1ff)) + 0x10000;
		ox = int((int64_t(st.dx) * f) >> 17);
		oy = int((int64_t(st.dy) * f) >> 17);
		b = st.dw ? int((int64_t(2 * st.dw) * f) >> 17) : 0;
		bh = st.dh ? int((int64_t(2 * st.dh) * f) >> 17) : 0;
	} else {
		ox = int(int64_t(st.dx) * t / d);
		oy = int(int64_t(st.dy) * t / d);
		b = st.dw ? int(int64_t(2 * st.dw) * t / d) : 0;
		bh = st.dh ? int(int64_t(2 * st.dh) * t / d) : 0;
	}
	ox += bounce(b, st.dw, st.xoff);
	oy += bounce(bh, st.dh, st.yoff);
}

}  // namespace fx
}  // namespace ore
