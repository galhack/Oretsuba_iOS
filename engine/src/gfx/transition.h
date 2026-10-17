// Screen transitions run on a page (docs/re/30_effects.md): the page shows a
// work image computed from its old content and its linked back page.
#pragma once

#include <cstdint>

#include "gfx/image.h"

namespace ore {

class Transition {
public:
	virtual ~Transition() = default;
	// Renders the frame for time `now` into `out` (same size as the page):
	// `from` = the page's own composed content, `to` = the linked page.
	// Returns true once complete.
	virtual bool render(int64_t now, const Image &from, const Image &to, Image &out) = 0;
	// The original allows ending a transition before it completed.
	bool done = false;
	int64_t start = -1;
};

}  // namespace ore
