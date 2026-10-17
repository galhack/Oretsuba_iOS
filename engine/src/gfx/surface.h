// Pages ("surfaces" types 7/9/10), their compositor, views and the screen
// (docs/re/20_graphics_core.md §2.3-3.4).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/object.h"
#include "gfx/image.h"
#include "gfx/layer.h"

namespace ore {

class Transition;

// Increments whenever anything visible may have changed.
extern uint64_t g_gfx_generation;
inline void gfx_touch() {
	++g_gfx_generation;
}

class Compositor {
public:
	Compositor() : main(this), overlay_storage(this) {}

	LayerList main;
	LayerList overlay_storage;
	LayerList *overlay = nullptr;  // points at the page's overlay list or null

	ImageRef bg;
	int bg_x = 0, bg_y = 0;
	uint32_t bg_color = 0;  // 0x00RRGGBB
	ImageRef bg_cache;      // zoomed/rotated background (drawn at 0,0)
	int rot_cx = 0, rot_cy = 0;
	bool disabled = false;
	bool no_bg = false;
	bool hide_main = false, hide_overlay = false;

	// Composes everything into target (full redraw).
	void compose(Image &target, int filter_mode = 0, int filter_tag = 0, bool with_overlay = true);
	void swap_background(Compositor &o);
};

class Surface : public Object {
public:
	Surface(int type, int w, int h, int bpp, int mode);
	~Surface() override;
	int gfx_type() const override { return type_; }

	Image &image() { return *image_; }
	ImageRef image_ref() { return image_; }
	// What a view shows: the transition work image while one runs.
	Image &current_image();

	Compositor *comp() { return comp_.get(); }
	LayerList *overlay_list() { return has_overlay_ ? &overlay_list_ : nullptr; }
	bool use_compositor = false;

	Surface *linked = nullptr;
	std::unique_ptr<Transition> transition;
	ImageRef work;
	int trans_mode = 0;
	bool trans_apply = false;

	// Recomposes the page if anything changed.
	void update(int64_t now);
	void invalidate() { dirty_ = true, gfx_touch(); }

	void swap_compositors(Surface &other);
	bool link(Surface *other);
	int start_transition(std::unique_ptr<Transition> t, int mode, bool apply);
	void end_transition(bool apply);
	bool transition_active() const { return transition != nullptr; }

private:
	int type_;
	ImageRef image_;
	std::unique_ptr<Compositor> comp_;
	LayerList overlay_list_;
	bool has_overlay_ = false;
	bool dirty_ = true;
	uint64_t composed_gen_ = 0;
};

class View : public Object {
public:
	View(Surface *s, const Rect *src, int x, int y);
	int gfx_type() const override { return 0; }
	Surface *surface;
	Rect src;
	int x, y;
	int dw = 0, dh = 0;
	bool visible = true;
	int prio = 0;
	int tag = 0;
	bool in_screen = false;
};

// Image object handed to scripts (type 4).
class ImageObj : public Object {
public:
	explicit ImageObj(ImageRef img) : image(std::move(img)) {}
	int gfx_type() const override { return 4; }
	ImageRef image;
};

}  // namespace ore
