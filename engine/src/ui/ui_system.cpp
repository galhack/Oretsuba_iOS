// Control factory and control group (docs/re/60_ui.md §4-5).
#include "ui/ui_system.h"

#include <algorithm>

#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/surface.h"
#include "text/font.h"
#include "ui/frame_anim.h"
#include "util/log.h"

namespace ore {

// ---------------------------------------------------------------- UiControl

UiControl::UiControl(UiSystem &ui) : ui_(ui) {}

UiControl::~UiControl() {
	ui_.forget(this);
}

// FUN_00448b70: disabling the focused control clears focus, the hovered one loses hover.
bool UiControl::set_enabled(bool e) {
	if (e == enabled_) return false;
	if (in_group && !e) {
		if (ui_.focus() == this) ui_.set_focus(nullptr);
		if (ui_.hover() == this) ui_.drop_hover();
	}
	enabled_ = e;
	return true;
}

bool UiControl::hit_test(int sx, int sy) const {
	return sx >= x() && sy >= y() && sx < x() + width() && sy < y() + height();
}

int UiControl::group_add(int pos) {
	return ui_.insert_node(this, pos);
}

void UiControl::group_remove() {
	ui_.erase_node(this);
}

// ---------------------------------------------------------------- parent

LayerList *UiParent::list() const {
	if (!page || !page->comp()) return nullptr;
	return &page->comp()->main;
}

// ---------------------------------------------------------------- UiSystem: factory

UiSystem::UiSystem(Engine &e) : engine(e) {}

UiSystem::~UiSystem() {
	destroying_ = true;
	hide_tooltip();
	group_.clear();
	saved_.clear();
	hover_ = focus_ = nullptr;
	controls_.clear();
	image_objs_.clear();
	frame_anims.clear();
}

UiControl *UiSystem::own(std::unique_ptr<UiControl> c) {
	c->tag = creation_tag;
	controls_.push_back(std::move(c));
	return controls_.back().get();
}

bool UiSystem::owns(const UiControl *c) const {
	for (auto &p : controls_)
		if (p.get() == c) return true;
	return false;
}

bool UiSystem::destroy(UiControl *c) {
	for (auto it = controls_.begin(); it != controls_.end(); ++it) {
		if (it->get() != c) continue;
		c->group_remove();
		std::unique_ptr<UiControl> doomed = std::move(*it);
		controls_.erase(it);
		doomed.reset();
		gfx_touch();
		return true;
	}
	return false;
}

void UiSystem::destroy_by_tag(int t, bool invert) {
	std::vector<UiControl *> doomed;
	for (auto &p : controls_)
		if ((p->tag == t) != invert) doomed.push_back(p.get());
	for (UiControl *c : doomed) destroy(c);
}

void UiSystem::destroy_all() {
	std::vector<UiControl *> doomed;
	for (auto &p : controls_) doomed.push_back(p.get());
	for (UiControl *c : doomed) destroy(c);
}

void UiSystem::set_parent(Surface *page) {
	parent_page = page;
	parent_handle_ = page ? page->handle() : 0;
	tip_page = page;
	tip_page_handle_ = parent_handle_;
	hide_tooltip();
}

Surface *UiSystem::parent_surface() const {
	Surface *page = parent_handle_ ? Object::as<Surface>(parent_handle_) : nullptr;
	return page && page == parent_page ? page : nullptr;
}

LayerList *UiSystem::parent_list() const {
	Surface *page = parent_surface();
	if (!page || !page->comp()) return nullptr;
	if (parent_b && page->overlay_list()) return page->overlay_list();
	return &page->comp()->main;
}

bool UiSystem::attach(Layer *l, int z) {
	LayerList *list = parent_list();
	if (!list || !l) return false;
	l->tag = creation_tag;
	list->insert(l, z, creation_tag);
	return true;
}

uint32_t UiSystem::image_handle(const ImageRef &img) {
	if (!img) return 0;
	for (auto it = image_objs_.begin(); it != image_objs_.end();) {
		if (it->first != img.get() && it->second->image.use_count() <= 1)
			it = image_objs_.erase(it);
		else
			++it;
	}
	auto &slot = image_objs_[img.get()];
	if (!slot) slot = std::make_unique<ImageObj>(img);
	return slot->handle();
}

// ---------------------------------------------------------------- group: lists

int UiSystem::insert_node(UiControl *c, int pos) {
	if (!c) return 1;
	if (c->in_group) return 2;
	c->in_group = true;
	if (pos < 0 || size_t(pos) >= group_.size())
		group_.push_back(c);
	else
		group_.insert(group_.begin() + pos, c);
	return 0;
}

// FUN_00449530: searches the current list, then the saved ones.
void UiSystem::erase_node(UiControl *c) {
	if (!c) return;
	if (hover_ == c) drop_hover();
	auto it = std::find(group_.begin(), group_.end(), c);
	if (it != group_.end()) {
		if (focus_ == c) {
			focus_ = nullptr;
			c->on_kill_focus(c);
		}
		group_.erase(it);
		c->in_group = false;
		return;
	}
	for (auto &list : saved_) {
		auto jt = std::find(list.begin(), list.end(), c);
		if (jt != list.end()) {
			list.erase(jt);
			c->in_group = false;
			return;
		}
	}
}

void UiSystem::forget(UiControl *c) {
	if (destroying_) return;
	if (hover_ == c) {
		hover_ = nullptr;
		hide_tooltip();
	}
	if (focus_ == c) focus_ = nullptr;
	group_.erase(std::remove(group_.begin(), group_.end(), c), group_.end());
	for (auto &list : saved_) list.erase(std::remove(list.begin(), list.end(), c), list.end());
	for (auto &p : controls_)
		for (UiControl *&n : p->neighbour)
			if (n == c) n = nullptr;
}

int UiSystem::add_to_group(UiControl *c, int pos) {
	if (!c) return 1;
	return c->group_add(pos);
}

void UiSystem::remove_from_group(UiControl *c) {
	if (c) c->group_remove();
}

void UiSystem::clear_group() {
	set_focus_node(nullptr, 1);
	drop_hover();
	for (UiControl *c : group_) c->in_group = false;
	group_.clear();
}

int UiSystem::push_group() {
	if (group_.empty()) return 1;
	reset();
	saved_.push_back(std::move(group_));
	group_.clear();
	return 0;
}

int UiSystem::pop_group() {
	if (saved_.empty()) return 1;
	clear_group();
	group_ = std::move(saved_.back());
	saved_.pop_back();
	// Re-evaluate the hover on the next update even if the pointer did not
	// move (a tap at the same spot after a modal dialog must still work).
	mouse_x_ = mouse_y_ = -1;
	return 0;
}

// ---------------------------------------------------------------- group: focus / hover

bool UiSystem::drop_hover() {
	if (!hover_) return false;
	UiControl *h = hover_;
	hover_ = nullptr;
	h->on_mouse_leave();
	hide_tooltip();
	return true;
}

void UiSystem::set_focus_node(UiControl *c, int dir) {
	if (c == focus_) return;
	UiControl *old = focus_;
	focus_ = c;
	drop_hover();
	if (old) old->on_kill_focus(c);
	if (c) c->on_set_focus(old, dir);
}

int UiSystem::focus_control(UiControl *c, int dir) {
	if (group_.empty()) return 1;
	if (!c) {
		set_focus_node(nullptr, dir);
		return 0;
	}
	if (std::find(group_.begin(), group_.end(), c) == group_.end()) return 2;
	if (!focusable(c)) return 3;
	set_focus_node(c, dir);
	return 0;
}

bool UiSystem::set_focus(UiControl *c, int dir) {
	return focus_control(c, dir ? dir : 1) == 0;
}

void UiSystem::on_hidden(UiControl *c) {
	if (focus_ == c) set_focus_node(nullptr, 1);
	if (hover_ == c) drop_hover();
}

// FUN_00448eb0
int UiSystem::focus_next() {
	if (group_.empty()) return 1;
	size_t start = 0;
	auto cur = std::find(group_.begin(), group_.end(), focus_);
	if (focus_ && cur != group_.end()) start = size_t(cur - group_.begin()) + 1;
	for (size_t i = start; i < group_.size(); ++i)
		if (focusable(group_[i])) {
			set_focus_node(group_[i], 1);
			return 0;
		}
	if (!focus_ || cur == group_.end()) return 2;
	for (size_t i = 0; i + 1 < start; ++i)
		if (focusable(group_[i])) {
			set_focus_node(group_[i], 1);
			return 0;
		}
	return -1;
}

// FUN_00448f40 (mirror of focus_next)
int UiSystem::focus_prev() {
	if (group_.empty()) return 1;
	auto cur = std::find(group_.begin(), group_.end(), focus_);
	size_t n = group_.size();
	size_t start = (focus_ && cur != group_.end()) ? size_t(cur - group_.begin()) : n;
	for (size_t i = start; i-- > 0;)
		if (focusable(group_[i])) {
			set_focus_node(group_[i], 1);
			return 0;
		}
	if (!focus_ || cur == group_.end()) return 2;
	for (size_t i = n; i-- > start + 1;)
		if (focusable(group_[i])) {
			set_focus_node(group_[i], 1);
			return 0;
		}
	return -1;
}

// FUN_00449780: 1 = previous (up/left or Shift+Tab), 2 = next (down/right or Tab).
int UiSystem::tab_direction() const {
	if (key_pressed(0x0f)) return key_held(0x2a) || key_held(0x36) ? 1 : 2;
	if (action_pressed(0) || action_pressed(2)) return 1;
	if (action_pressed(1) || action_pressed(3)) return 2;
	return 0;
}

// FUN_00449ba0
bool UiSystem::apply_tab(UiControl *f, int dir) {
	if (dir == 0) return false;
	int r = dir == 1 ? focus_prev() : focus_next();
	if (r == 0) return true;
	if (r == -1 && f) {
		f->on_set_focus(f, dir);  // nothing else focusable: re-focus self
		return true;
	}
	return false;
}

void UiSystem::park_mouse() {
	engine.input.warp(Engine::kWidth - 1, Engine::kHeight - 1);
}

// ---------------------------------------------------------------- group: update

void UiSystem::reset() {
	event_id_ = -1;
	set_focus_node(nullptr, 1);
	drop_hover();
	hide_tooltip();
	hover_time_ = 0;
	mouse_x_ = mouse_y_ = -1;
}

int UiSystem::take_event() {
	int id = event_id_;
	event_id_ = -1;
	return id;
}

// FUN_00449820 (§5.3)
int UiSystem::update(int64_t now) {
	for (auto &c : controls_) c->tick(now);
	if (!active_) return last_result = -1;
	int mx = engine.input.cursor_x(), my = engine.input.cursor_y();
	bool moved = mx != mouse_x_ || my != mouse_y_;
	if (moved) {
		if (hover_time_) hover_time_ = now;
		UiControl *node = nullptr;
		if (!capture_) {
			for (UiControl *c : group_)
				if (c->enabled() && c->visible() && c->hit_test(mx, my)) {
					node = c;
					break;
				}
			mouse_x_ = mx, mouse_y_ = my;
			if (node != hover_) {
				if (hover_) hover_->on_mouse_leave();
				if (focus_ && focus_ != node) focus_->on_mouse_leave();
				hover_ = node;
				if (node) node->on_mouse_enter();
				hide_tooltip();
				hover_time_ = now;
			}
		} else {
			node = hover_;
			mouse_x_ = mx, mouse_y_ = my;
		}
		if (node) node->on_mouse_move(mx - node->x(), my - node->y());
		if (tip_shown_) place_tooltip(mx, my);
	} else if (hover_) {
		const std::string &tip = hover_->tooltip;
		if (!tip.empty() && !tip_shown_ && tip_enabled && hover_time_) {
			int64_t t = now - hover_time_;
			if (t >= tip_delay && (tip_duration == 0 || t <= tip_delay + tip_duration)) show_tooltip(tip, mx, my);
		} else if (tip_shown_ && tip_duration && now - hover_time_ > tip_delay + tip_duration) {
			hide_tooltip();
		}
	}
	if (hover_ && hover_->on_mouse(mouse_x_ - hover_->x(), mouse_y_ - hover_->y())) {
		hide_tooltip();
		return last_result = 1;
	}
	UiControl *f = focus_;
	if (f && f->on_text_input()) return last_result = 3;
	if (f) {
		if (f->on_key()) return last_result = 2;
		for (int a = 0; a < 4; ++a)
			if (f->neighbour[a] && action_pressed(a) && focus_control(f->neighbour[a], 1) == 0) return last_result = 4;
	}
	if (apply_tab(f, tab_direction())) return last_result = 4;
	return last_result = 0;
}

// ---------------------------------------------------------------- tooltips (§5.5)

void UiSystem::show_tooltip(const std::string &text, int mx, int my) {
	hide_tooltip();
	Font *font = tip_font ? Object::as<Font>(tip_font) : nullptr;
	if (!font) font = engine.default_font;
	Surface *page = tip_page_handle_ ? Object::as<Surface>(tip_page_handle_) : nullptr;
	if (!font || !page || !page->comp()) return;
	int tw = 0, th = 0;
	font->measure(text, tw, th);
	if (tw <= 0 || th <= 0) th = font->size, tw = tw > 0 ? tw : font->size;
	int bw = tw + 6, bh = th + 6;
	auto img = std::make_shared<Image>(bw + 4, bh + 4, 32);
	img->fill(0);
	fill_rect(*img, Rect(4, 4, bw + 4, bh + 4), 0x80000000);  // drop shadow
	fill_rect(*img, Rect(0, 0, bw, bh), 0xFF445F97);          // border
	fill_rect(*img, Rect(1, 1, bw - 1, bh - 1), 0xFFFFFFE1);  // face
	img->has_alpha = true;
	font->draw(*img, 3, 3, text, true);
	tip_sprite_ = std::make_unique<Sprite>(img, nullptr, 0, 0);
	tip_sprite_->set_mode(kBlendAlpha);
	LayerList *list = page->overlay_list() ? page->overlay_list() : &page->comp()->main;
	list->insert(tip_sprite_.get(), -0x40000000, 0);
	tip_shown_ = true;
	place_tooltip(mx, my);
}

// FUN_00408970: below the cursor, clamped to the page.
void UiSystem::place_tooltip(int mx, int my) {
	if (!tip_sprite_) return;
	Surface *page = tip_page_handle_ ? Object::as<Surface>(tip_page_handle_) : nullptr;
	int w = tip_sprite_->src.w(), h = tip_sprite_->src.h();
	int x = mx, y = my + tip_offset_y;
	if (page) {
		if (x + w > page->image().width()) x = page->image().width() - w;
		if (y + h > page->image().height()) y = my - h;
	}
	tip_sprite_->set_pos(x, y);
}

void UiSystem::hide_tooltip() {
	tip_shown_ = false;
	if (tip_sprite_) {
		tip_sprite_->detach();
		tip_sprite_.reset();
	}
}

// ---------------------------------------------------------------- misc

// FUN_00408d00: slots -4/-3/-2 (click/hover/leave) are never configured in
// this game, so controls are silent; explicit indices are unused.
void UiSystem::play_se(int idx) {
	(void)idx;
}

void UiSystem::drop_hover_focus() {
	set_focus_node(nullptr, 1);
	drop_hover();
}

bool UiSystem::key_pressed(int dik) const {
	return engine.input.key_pressed(dik);
}

bool UiSystem::key_held(int dik) const {
	return engine.input.key_held(dik);
}

// Key-action mapper app+0x1904 (same mapping as N_220 for buttons 0..4).
bool UiSystem::action_pressed(int action) const {
	const Input &in = engine.input;
	switch (action) {
	case 0: return in.key_pressed(0xC8);
	case 1: return in.key_pressed(0xD0);
	case 2: return in.key_pressed(0xCB);
	case 3: return in.key_pressed(0xCD);
	case 4: {
		int k = engine.opt8(0xeaa);
		return in.key_pressed(0x1C) || in.key_pressed(0x9C) || (k && in.key_pressed(k));
	}
	default: return false;
	}
}

bool UiSystem::mouse_pressed(int b) const {
	return engine.input.mouse_pressed(b);
}

bool UiSystem::mouse_held(int b) const {
	return engine.input.mouse_held(b);
}

int64_t UiSystem::now() const {
	return engine.now;
}

}  // namespace ore
