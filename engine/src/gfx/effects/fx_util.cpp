#include "gfx/effects/fx_util.h"

#include <cmath>
#include <cstdlib>
#include <random>
#include <algorithm>
#include <cstring>

#include "core/engine.h"

namespace ore {
namespace fx {
namespace {

struct Tables {
	int ic[512];
	double dc[512];
	Tables() {
		for (int i = 0; i < 512; ++i) {
			dc[i] = std::cos(i * M_PI / 256.0);
			ic[i] = int(std::trunc(dc[i] * 65536.0));
		}
	}
};
const Tables &tables() {
	static Tables t;
	return t;
}

}  // namespace

int icos(int i) {
	return tables().ic[i & 511];
}

double dcos(int i) {
	return tables().dc[i & 511];
}

double ease_p(double p, double e) {
	if (e < 0) return 1.0 - std::pow(1.0 - p, -e);
	if (e > 0) return std::pow(p, e);
	return p;
}

int64_t ease_ms(int64_t t, int64_t d, double e) {
	if (d <= 0 || e == 0) return t;
	return int64_t(std::trunc(double(d) * ease_p(double(t) / double(d), e)));
}

double atof_or(const char *s, double def) {
	return s && *s ? std::atof(s) : def;
}

uint32_t rnd() {
	static std::mt19937 gen(1993);
	return gen();
}

double rnd01() {
	return (rnd() % 1000000000u) / 1e9;
}

void hblur(Image &dst, const Image &src, int r) {
	const int w = std::min(dst.width(), src.width()), h = std::min(dst.height(), src.height());
	if (r <= 0 || w <= 0) {
		for (int y = 0; y < h; ++y) memcpy(dst.row32(y), src.row32(y), size_t(w) * 4);
		return;
	}
	const uint32_t k = 65536u / uint32_t(2 * r + 1);
	for (int y = 0; y < h; ++y) {
		const uint32_t *s = src.row32(y);
		uint32_t *d = dst.row32(y);
		uint32_t sum[4] = {0, 0, 0, 0};
		auto px = [&](int x) { return s[x < 0 ? 0 : x >= w ? w - 1 : x]; };
		for (int i = -r; i <= r; ++i) {
			uint32_t p = px(i);
			for (int c = 0; c < 4; ++c) sum[c] += (p >> (c * 8)) & 0xff;
		}
		for (int x = 0; x < w; ++x) {
			uint32_t o = 0;
			for (int c = 0; c < 4; ++c) o |= ((sum[c] * k + 0x8000) >> 16 & 0xff) << (c * 8);
			d[x] = o;
			uint32_t pa = px(x + r + 1), pr = px(x - r);
			for (int c = 0; c < 4; ++c) sum[c] += ((pa >> (c * 8)) & 0xff) - ((pr >> (c * 8)) & 0xff);
		}
	}
}

Compositor *find_comp(Engine &e, Surface *hint, const ImageRef &bg, const ImageRef &cache) {
	std::vector<Surface *> order;
	if (hint) order.push_back(hint);
	if (hint && hint->linked) order.push_back(hint->linked);
	for (auto &o : e.images)
		if (auto *s = dynamic_cast<Surface *>(o.get())) order.push_back(s);
	if (cache)
		for (Surface *s : order)
			if (s->comp() && s->comp()->bg_cache == cache) return s->comp();
	if (bg)
		for (Surface *s : order)
			if (s->comp() && s->comp()->bg == bg) return s->comp();
	return hint ? hint->comp() : nullptr;
}

}  // namespace fx
}  // namespace ore
