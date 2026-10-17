#include "gfx/surface.h"

#include <algorithm>

#include "gfx/blend.h"
#include "gfx/transition.h"

namespace ore {

uint64_t g_gfx_generation = 1;

// ---------------------------------------------------------------- Compositor

void Compositor::compose(Image &target, int filter_mode, int filter_tag, bool with_overlay) {
	const Rect full = target.rect();
	if (!no_bg) {
		if (bg_cache) {
			copy_rect(target, 0, 0, *bg_cache, Rect(0, 0, bg_cache->width(), bg_cache->height()));
		} else if (bg) {
			// opaque copy of the visible window of the background; parts outside are left as they were
			copy_rect(target, 0, 0, *bg, Rect(bg_x, bg_y, bg_x + full.w(), bg_y + full.h()));
		} else {
			fill_rect(target, full, target.bpp() == 8 ? (bg_color & 0xff) : (bg_color & 0xffffff));
		}
	}
	auto draw_list = [&](LayerList *l) {
		if (!l) return;
		auto &n = l->nodes();
		for (size_t i = n.size(); i-- > 0;) {
			Layer *ly = n[i].layer;
			if (!ly->visible || ly->opacity <= 0) continue;
			if (filter_mode == 1 && ly->tag != filter_tag) continue;
			if (filter_mode == 2 && ly->tag == filter_tag) continue;
			ly->draw(target, full);
		}
	};
	if (!hide_main) draw_list(&main);
	if (with_overlay && !hide_overlay) draw_list(overlay);
}

void Compositor::swap_background(Compositor &o) {
	std::swap(bg, o.bg);
	std::swap(bg_x, o.bg_x);
	std::swap(bg_y, o.bg_y);
	std::swap(bg_color, o.bg_color);
	std::swap(bg_cache, o.bg_cache);
	std::swap(rot_cx, o.rot_cx);
	std::swap(rot_cy, o.rot_cy);
	gfx_touch();
}

// ---------------------------------------------------------------- Surface

Surface::Surface(int type, int w, int h, int bpp, int mode)
    : type_(type), image_(std::make_shared<Image>(w, h, bpp)), overlay_list_(nullptr) {
	if (mode != 0) {
		comp_ = std::make_unique<Compositor>();
		use_compositor = true;
		if (mode == 2 && type != 10) {
			has_overlay_ = true;
			overlay_list_.set_owner(comp_.get());
			comp_->overlay = &overlay_list_;
		}
	}
}

Surface::~Surface() {
	if (linked && linked->linked == this) linked->linked = nullptr;
	// layers detach through their list destructors
}

Image &Surface::current_image() {
	return transition && work ? *work : *image_;
}

void Surface::update(int64_t now) {
	if (use_compositor && comp_ && !comp_->disabled && (composed_gen_ != g_gfx_generation || dirty_)) {
		comp_->compose(*image_);
		composed_gen_ = g_gfx_generation;
		dirty_ = false;
	}
	if (transition && linked) {
		linked->update(now);
		if (transition->start < 0) transition->start = now;
		if (transition->render(now, *image_, linked->image(), *work)) transition->done = true;
		gfx_touch();
	}
}

void Surface::swap_compositors(Surface &other) {
	if (!comp_ || !other.comp_) return;
	// exchange main lists and backgrounds; overlay lists stay with their pages
	LayerList::swap_contents(comp_->main, other.comp_->main);
	comp_->swap_background(*other.comp_);
	std::swap(comp_->no_bg, other.comp_->no_bg);
	std::swap(comp_->hide_main, other.comp_->hide_main);
	gfx_touch();
}

bool Surface::link(Surface *other) {
	if (!other) {
		linked = nullptr;
		return true;
	}
	if (other->image_->width() != image_->width() || other->image_->height() != image_->height() ||
	    other->image_->bpp() != image_->bpp())
		return false;
	linked = other;
	return true;
}

int Surface::start_transition(std::unique_ptr<Transition> t, int mode, bool apply) {
	if (!comp_) return 1;
	if (!linked) return 2;
	if (transition) end_transition(trans_apply);
	transition = std::move(t);
	trans_mode = mode;
	trans_apply = apply;
	if (!work || work->width() != image_->width() || work->height() != image_->height())
		work = std::make_shared<Image>(image_->width(), image_->height(), image_->bpp());
	gfx_touch();
	return 0;
}

void Surface::end_transition(bool apply) {
	if (!transition) return;
	transition.reset();
	if (apply && linked) {
		if (trans_mode == 1) swap_compositors(*linked);
		if (trans_mode == 2) comp_->swap_background(*linked->comp_);
	}
	invalidate();
}

// ---------------------------------------------------------------- View

View::View(Surface *s, const Rect *r, int nx, int ny) : surface(s), x(nx), y(ny) {
	src = r ? *r : s->image().rect();
}

}  // namespace ore
