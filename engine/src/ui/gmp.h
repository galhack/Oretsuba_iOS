// GMP layout files (docs/re/60_ui.md §7): the N_208 loader (DAT_00503268).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gfx/image.h"

namespace ore {

class UiSystem;

class GmpLoader {
public:
	struct Entry {
		uint32_t handle = 0;  // control
		int w = 0, h = 0;     // one frame
		int frames = 0;
	};

	// FUN_0040e8a0: builds the controls of `path` through the factory, adds
	// them to the group and numbers them from start_id (< 0 = no ids).
	// Returns 0 ok, 1 bad args, 2 open failure, 3 bad magic, 4 background
	// load, 5 no parent, 6..0xb control init, 0xc bad type.
	int load(UiSystem &ui, const std::string &path, int start_id, int z, bool ck_mode);
	void clear();

	ImageRef bg;
	std::vector<Entry> entries;
};

}  // namespace ore
