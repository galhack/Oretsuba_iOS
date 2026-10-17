#include "ui/gmp.h"

#include <cstring>

#include "core/engine.h"
#include "gfx/surface.h"
#include "ui/controls.h"
#include "util/log.h"

namespace ore {

void GmpLoader::clear() {
	bg.reset();
	entries.clear();
}

int GmpLoader::load(UiSystem &ui, const std::string &path, int start_id, int z, bool ck_mode) {
	clear();
	if (path.empty()) return 1;
	Engine &e = ui.engine;
	std::vector<uint8_t> d;
	if (!e.read_file(path, d)) {
		log_warn("gmp: cannot open %s", path.c_str());
		return 2;
	}
	size_t p = 0;
	auto u8 = [&]() -> int { return p < d.size() ? d[p++] : 0; };
	auto s16 = [&]() -> int {
		int v = p + 1 < d.size() ? int16_t(d[p] | (d[p + 1] << 8)) : 0;
		p += 2;
		return v;
	};
	auto str = [&](int n) {
		std::string s;
		for (int i = 0; i < n && p < d.size(); ++i) s.push_back(char(d[p++]));
		size_t z0 = s.find('\0');
		if (z0 != std::string::npos) s.resize(z0);
		return s;
	};
	if (d.size() < 7 || memcmp(d.data(), "LGM\0", 4) != 0) return 3;
	p = 4;
	std::string dir;
	size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos) dir = path.substr(0, slash + 1);
	int ck = ck_mode ? 0xff00 : -1;
	int blend = ck_mode ? 0 : 2;

	// background: becomes the parent compositor's background image (FUN_0043a9c0)
	int bg_len = u8();
	if (bg_len > 0) {
		std::string name = str(bg_len);
		Surface *page = ui.parent_surface();
		if (!page || !page->comp()) return 5;
		bg = load_control_image(e, dir + name, ck, -1);
		if (!bg) return 4;
		Compositor *c = page->comp();
		c->bg = bg;
		c->bg_x = c->bg_y = 0;
		c->bg_cache.reset();
		c->rot_cx = bg->width() >> 1;
		c->rot_cy = bg->height() >> 1;
		page->invalidate();
	}

	int count = d.size() >= p + 2 ? (d[p] | (d[p + 1] << 8)) : 0;
	p += 2;
	CheckButton *leader = nullptr;
	int last_type = -1;
	for (int i = 0; i < count; ++i) {
		std::string name = str(u8());
		int type = u8();
		int x = s16(), y = s16();
		int alpha = u8();
		bool visible = u8() != 0;
		int w = s16(), h = s16();
		int frames = u8();
		if (p > d.size()) break;
		std::string file = dir + name;
		std::unique_ptr<UiControl> ctrl;
		int err = 0;
		switch (type) {
		case 1:
		case 2:
		case 3: {
			auto b = std::make_unique<Button>(ui);
			bool has_dis = type == 3 ? frames == 3 : frames == 4;
			if (!b->init_file(file, has_dis, type != 3, x, y, blend, alpha, ck, -1, z)) err = type == 3 ? 7 : 6;
			ctrl = std::move(b);
			break;
		}
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 9: {
			static const int kStyle[10] = {0, 0, 0, 0, 0, 1, 2, 3, 2, 3};
			static const int kDisFrames[10] = {0, 0, 0, 0, 8, 8, 6, 5, 6, 5};
			bool radio = type >= 6;
			CheckButton *lead = radio && type == last_type ? leader : nullptr;
			auto c = std::make_unique<CheckButton>(ui);
			if (!c->init_file(file, kStyle[type], frames == kDisFrames[type], lead, x, y, blend, alpha, ck, -1, z))
				err = 8;
			if (radio && !lead) leader = c.get();
			ctrl = std::move(c);
			break;
		}
		default:
			err = 0xc;
			break;
		}
		last_type = type;
		if (err) {
			log_warn("gmp: %s entry %d (%s, type %d) failed", path.c_str(), i, name.c_str(), type);
			return err;
		}
		UiControl *c = ui.own(std::move(ctrl));
		entries.push_back({c->handle(), w, h, frames});
		c->set_visible(visible);
		ui.add_to_group(c, -1);
		if (start_id >= 0) c->id = start_id++;
	}
	return 0;
}

}  // namespace ore
