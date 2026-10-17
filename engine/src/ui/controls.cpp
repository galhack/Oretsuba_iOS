// UI controls (docs/re/60_ui.md §6).
#include "ui/controls.h"

#include <cstdlib>
#include <cstring>

#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/surface.h"
#include "text/font.h"
#include "util/log.h"

namespace ore {

// ---------------------------------------------------------------- helpers

ImageRef load_control_image(Engine &e, const std::string &path, int ck, int ck2) {
	if (path.empty()) return nullptr;
	ImageRef img = e.load_image(path, false);
	if (!img) return nullptr;
	if (ck2 >= 0) {
		if (ck >= 0 && ck < img->width() && ck2 < img->height()) img->color_key = img->pixel(ck, ck2);
	} else if (ck != -1) {
		img->color_key = uint32_t(ck);
	}
	return img;
}

static bool pixel_hit_possible(const Image &img) {
	return img.has_alpha || img.color_key != 0xFFFFFFFF || img.bpp() == 8;
}

// ---------------------------------------------------------------- SpriteControl

SpriteControl::~SpriteControl() = default;

bool SpriteControl::init_sprite(ImageRef img, int frames, int nx, int ny, int z, int blend, int alpha) {
	if (!img) return false;
	image_ = std::move(img);
	frames_ = frames < 1 ? 1 : frames;
	fw_ = image_->width() / frames_;
	fh_ = image_->height();
	Rect r(0, 0, fw_, fh_);
	sprite_ = std::make_unique<Sprite>(image_, &r, nx, ny);
	sprite_->set_mode(blend);
	sprite_->set_opacity(alpha);
	z_ = z;
	if (!ui_.attach(sprite_.get(), z)) log_warn("ui: no parent page for a type %d control", ui_type());
	return true;
}

void SpriteControl::show_frame(int f) {
	if (!sprite_) return;
	if (f < 0 || f >= frames_) f = 0;
	Rect r(fw_ * f, 0, fw_ * (f + 1), fh_);
	if (sprite_->src == r) return;
	sprite_->src = r;
	sprite_->touch();
}

void SpriteControl::set_pos(int nx, int ny) {
	if (sprite_) sprite_->set_pos(nx, ny);
}

// FUN_004c7320: hiding drops focus; hover is dropped either way.
void SpriteControl::set_visible(bool v) {
	if (!v) ui_.on_hidden(this);
	else if (ui_.hover() == this) ui_.drop_hover();
	if (sprite_) sprite_->set_visible(v);
}

// FUN_004c7380: re-attach at a new priority when attached.
void SpriteControl::set_z(int z) {
	z_ = z;
	if (sprite_ && sprite_->list()) sprite_->list()->reprioritize(sprite_.get(), z, true);
}

// FUN_004d97d0: rectangle, or the alpha/colour key of the current frame.
bool SpriteControl::hit_test(int sx, int sy) const {
	if (!sprite_) return false;
	if (rect_hit_ || !pixel_hit_possible(*image_))
		return sx >= x() && sy >= y() && sx < x() + fw_ && sy < y() + fh_;
	return sprite_->hit(sx, sy, true);
}

// ---------------------------------------------------------------- Button (§6.1)

bool Button::init_file(const std::string &path, bool has_dis, bool has_pressed, int nx, int ny, int blend, int alpha,
                       int ck, int ck2, int z) {
	ImageRef img = load_control_image(ui_.engine, path, ck, ck2);
	if (!img) return false;
	if (ck2 < 0 && ck == -1 && blend == 0) rect_hit_ = true;
	return init_image(img, has_dis, has_pressed, nx, ny, blend, alpha, z);
}

bool Button::init_image(ImageRef img, bool has_dis, bool has_pressed, int nx, int ny, int blend, int alpha, int z) {
	has_dis_ = has_dis;
	has_pressed_ = has_pressed;
	if (!init_sprite(std::move(img), 2 + has_pressed + has_dis, nx, ny, z, blend, alpha)) return false;
	set_state(0);
	return true;
}

void Button::set_state(int s) {
	state_ = s;
	int f = s;
	if (s == 3 && !has_dis_) f = 0;
	else if (s >= 2 && !has_pressed_) f = s - 1;
	show_frame(f);
}

bool Button::set_enabled(bool e) {
	if (!UiControl::set_enabled(e)) return false;
	set_state(e ? 0 : 3);
	return true;
}

void Button::on_mouse_move(int, int) {
	if (state_ == 0) {
		set_state(1);
		ui_.play_se(-3);
	}
}

void Button::on_mouse_leave() {
	if (state_ == 1 || state_ == 2) {
		set_state(0);
		ui_.play_se(-2);
	}
}

// FUN_004d9930: press edge → pressed; release while pressed → fire.
bool Button::on_mouse(int, int) {
	if (state_ == 1) {
		if (ui_.mouse_pressed(0)) {
			set_state(2);
			key_timer_ = 0;
			return true;
		}
	} else if (state_ == 2 && key_timer_ == 0 && !ui_.mouse_held(0)) {
		set_state(1);
		do_fire();
		return true;
	}
	return false;
}

// FUN_004d99a0 (focus policy 2: park the mouse)
void Button::on_set_focus(UiControl *, int) {
	if (state_ == 0) {
		set_state(1);
		ui_.play_se(-3);
	}
	ui_.park_mouse();
}

void Button::on_kill_focus(UiControl *) {
	if (state_ == 1 || state_ == 2) set_state(0);
}

// FUN_004d9ab0: decide fires (after showing the pressed frame for 100 ms).
bool Button::on_key() {
	if (state_ == 1) {
		key_timer_ = 0;
		if (!ui_.action_pressed(4)) return false;
		if (!has_pressed_) {
			do_fire();
			return true;
		}
		set_state(2);
		key_timer_ = ui_.now() + 100;
		return true;
	}
	if (state_ == 2) {
		if (key_timer_ != 0 && ui_.now() > key_timer_) {
			set_state(1);
			do_fire();
			key_timer_ = 0;
		}
		return true;
	}
	key_timer_ = 0;
	return false;
}

void Button::do_fire() {
	ui_.fire(this);
	ui_.play_se(-4);
}

void ArrowButton::do_fire() {
	ui_.play_se(-4);
	if (owner_) owner_->step(dir_ & 1 ? 1 : -1);
}

// ---------------------------------------------------------------- CheckButton (§6.2)

// Frame table at 0x4f0f70: frame = T[style][state]; T[style][6] = frame
// count without the two disabled frames.
static const uint8_t kCheckFrames[4][8] = {
    {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 1, 2, 3, 3, 4, 5}, {0, 1, 2, 3, 3, 3, 4, 5}, {0, 1, 1, 2, 2, 2, 3, 4}};

CheckButton::~CheckButton() {
	if (leader_) {
		auto &m = leader_->members_;
		m.erase(std::remove(m.begin(), m.end(), this), m.end());
	}
	for (CheckButton *m : members_)
		if (m != this) m->leader_ = nullptr;
}

bool CheckButton::init_file(const std::string &path, int style, bool has_dis, CheckButton *leader, int nx, int ny,
                            int blend, int alpha, int ck, int ck2, int z) {
	ImageRef img = load_control_image(ui_.engine, path, ck, ck2);
	if (!img) return false;
	if (ck2 < 0 && ck == -1 && blend == 0) rect_hit_ = true;
	return init_image(img, style, has_dis, leader, nx, ny, blend, alpha, z);
}

bool CheckButton::init_image(ImageRef img, int style, bool has_dis, CheckButton *leader, int nx, int ny, int blend,
                             int alpha, int z) {
	style_ = style & 3;
	has_dis_ = has_dis;
	if (!init_sprite(std::move(img), kCheckFrames[style_][6] + (has_dis ? 2 : 0), nx, ny, z, blend, alpha))
		return false;
	int s = 0;
	if (radio()) {
		if (leader && leader->radio()) {
			leader_ = leader->leader_ ? leader->leader_ : leader;
			leader_->members_.push_back(this);
		} else {
			members_.push_back(this);  // first radio of a group: the leader, starts checked
			selected_ = 0;
			s = 3;
		}
	}
	set_state(s);
	return true;
}

void CheckButton::set_state(int s) {
	state_ = s;
	int f = kCheckFrames[style_][s & 7];
	if (s >= 6 && !has_dis_) f = kCheckFrames[style_][s == 6 ? 0 : 3];
	show_frame(f);
}

void CheckButton::uncheck_others(CheckButton *keep) {
	CheckButton *lead = leader_ ? leader_ : this;
	for (size_t i = 0; i < lead->members_.size(); ++i) {
		CheckButton *m = lead->members_[i];
		if (m == keep) {
			lead->selected_ = int(i);
			continue;
		}
		if (m->checked()) m->set_state(m->enabled() ? 0 : 6);
	}
}

void CheckButton::set_checked(bool c) {
	int off = state_ < 6 ? state_ % 3 : 0;
	if (off == 2) off = 1;
	if (!enabled()) set_state(c ? 7 : 6);
	else set_state((c ? 3 : 0) + off);
	if (c && radio()) uncheck_others(this);
}

void CheckButton::select_index(int i) {
	if (!radio()) {
		set_checked(i != 0);
		return;
	}
	CheckButton *lead = leader_ ? leader_ : this;
	for (size_t k = 0; k < lead->members_.size(); ++k) {
		CheckButton *m = lead->members_[k];
		bool on = int(k) == i;
		m->set_state(m->enabled() ? (on ? 3 : 0) : (on ? 7 : 6));
	}
	lead->selected_ = i;
}

int CheckButton::selected_index() const {
	const CheckButton *lead = leader_ ? leader_ : this;
	return lead->selected_;
}

bool CheckButton::set_enabled(bool e) {
	bool c = checked();
	if (!UiControl::set_enabled(e)) return false;
	set_state(e ? (c ? 3 : 0) : (c ? 7 : 6));
	return true;
}

void CheckButton::fire() {
	ui_.fire(this);
	ui_.play_se(-4);
}

// FUN_004da920
void CheckButton::toggle() {
	if (radio()) {
		set_state(3 + 1);
		uncheck_others(this);
	} else {
		set_state((checked() ? 0 : 3) + 1);
	}
	fire();
}

void CheckButton::on_mouse_move(int, int) {
	if (state_ == base()) {
		set_state(base() + 1);
		ui_.play_se(-3);
	}
}

void CheckButton::on_mouse_leave() {
	if (state_ < 6 && state_ % 3 != 0) {
		set_state(base());
		ui_.play_se(-2);
	}
}

bool CheckButton::on_mouse(int, int) {
	if (inactive_) return false;
	if (state_ == base() + 1) {
		if (ui_.mouse_pressed(0)) {
			set_state(base() + 2);
			key_timer_ = 0;
			return true;
		}
	} else if (state_ < 6 && state_ == base() + 2 && key_timer_ == 0 && !ui_.mouse_held(0)) {
		toggle();
		return true;
	}
	return false;
}

void CheckButton::on_set_focus(UiControl *, int) {
	if (state_ == base()) {
		set_state(base() + 1);
		ui_.play_se(-3);
	}
	ui_.park_mouse();
}

void CheckButton::on_kill_focus(UiControl *) {
	if (state_ < 6 && state_ % 3 != 0) set_state(base());
}

bool CheckButton::on_key() {
	if (inactive_ || state_ >= 6) return false;
	if (state_ == base() + 1) {
		key_timer_ = 0;
		if (!ui_.action_pressed(4)) return false;
		if (style_ & 1) {
			toggle();
			return true;
		}
		set_state(base() + 2);
		key_timer_ = ui_.now() + 100;
		return true;
	}
	if (state_ == base() + 2) {
		if (key_timer_ != 0 && ui_.now() > key_timer_) {
			key_timer_ = 0;
			toggle();
		}
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- EditBox (§6.3)

EditBox::~EditBox() {
	if (buffer_) ui_.engine.vm.mem().free(buffer_);
}

bool EditBox::init(Font *font, int lines, int width, int nx, int ny, int padding, int spacing, int border, int z,
                   uint32_t bg_color) {
	if (!font || lines < 1 || width < 1) return false;
	font_ = font->handle();
	lines_ = lines;
	padding_ = padding;
	spacing_ = spacing;
	border_ = border ? 1 : 0;
	int fh = std::abs(font->size);
	if (fh <= 0) fh = 16;
	line_h_ = fh + spacing;
	int inner = width - 2 * (padding + border_);
	per_line_ = size_t(inner > 0 ? inner / fh : 1) * 2;
	if (per_line_ == 0) per_line_ = 2;
	capacity_ = (per_line_ + 1) * size_t(lines);
	bg_ = bg_color;
	int h = lines * line_h_ - spacing + 2 * (padding + border_);
	auto img = std::make_shared<Image>(width, h > 0 ? h : fh, 32);
	img->has_alpha = true;
	if (!init_sprite(img, 1, nx, ny, z, kBlendAlpha, 255)) return false;
	rect_hit_ = true;
	buffer_ = ui_.engine.vm.mem().alloc(capacity_ + 1, "editbox");
	redraw();
	return true;
}

void EditBox::set_text(const std::string &s) {
	TextEncoding enc = ui_.engine.encoding;
	std::string t;
	for (size_t i = 0; i < s.size() && t.size() < capacity_;) {
		size_t n = is_lead_byte(uint8_t(s[i]), enc) && i + 1 < s.size() ? 2 : 1;
		if (t.size() + n > capacity_) break;
		t.append(s, i, n);
		i += n;
	}
	text_ = t;
	if (buffer_) {
		uint8_t *d = ui_.engine.vm.mem().host(buffer_, capacity_ + 1);
		memset(d, 0, capacity_ + 1);
		memcpy(d, text_.data(), text_.size());
	}
	redraw();
}

void EditBox::set_value_str(const char *s) {
	set_text(s ? s : "");
}

void EditBox::redraw() {
	if (!image_) return;
	Image &img = *image_;
	fill_rect(img, img.rect(), bg_);
	if (border_) {
		int w = img.width(), h = img.height();
		fill_rect(img, Rect(0, 0, w, 1), 0x80000000);
		fill_rect(img, Rect(0, 0, 1, h), 0x80000000);
		fill_rect(img, Rect(0, h - 1, w, h), 0x80ffffff);
		fill_rect(img, Rect(w - 1, 0, w, h), 0x80ffffff);
	}
	Font *font = Object::as<Font>(font_);
	if (font) {
		// wrap at per_line_ bytes and at line breaks
		TextEncoding enc = ui_.engine.encoding;
		std::vector<std::string> lines(1);
		for (size_t i = 0; i < text_.size();) {
			uint8_t c = uint8_t(text_[i]);
			if (c == '\r' || c == '\n') {
				i += (c == '\r' && i + 1 < text_.size() && text_[i + 1] == '\n') ? 2 : 1;
				lines.emplace_back();
				continue;
			}
			size_t n = is_lead_byte(c, enc) && i + 1 < text_.size() ? 2 : 1;
			if (lines.back().size() + n > per_line_) lines.emplace_back();
			lines.back().append(text_, i, n);
			i += n;
		}
		int off = padding_ + border_;
		for (size_t k = 0; k < lines.size() && int(k) < lines_; ++k)
			if (!lines[k].empty()) font->draw(img, off, off + int(k) * line_h_, lines[k], true);
	}
	if (sprite_) sprite_->touch();
}

// Touch devices have no IME: typed text comes from the platform text-entry UI.
void EditBox::begin_input() {
	if (input_pending_ || inactive_) return;
	Engine &e = ui_.engine;
	e.platform.begin_text_input(e.platform.to_utf8(text_, e.encoding));
	input_pending_ = true;
}

bool EditBox::on_mouse(int, int) {
	if (!ui_.mouse_pressed(0)) return false;
	if (!focused_ && in_group && ui_.set_focus(this)) return true;
	begin_input();
	return true;
}

void EditBox::on_set_focus(UiControl *, int) {
	focused_ = true;
	begin_input();
}

void EditBox::on_kill_focus(UiControl *) {
	focused_ = false;
}

bool EditBox::on_text_input() {
	if (!input_pending_) return false;
	Engine &e = ui_.engine;
	std::string utf8;
	int r = e.platform.poll_text_input(utf8);
	if (r < 0) return false;
	input_pending_ = false;
	if (r != 1) return false;
	set_text(e.platform.from_utf8(utf8, e.encoding));
	return true;
}

// ---------------------------------------------------------------- ScrollBar (§6.4)

ScrollBar::~ScrollBar() = default;

bool ScrollBar::init(int nx, int ny, int track_dx, int track_dy, const std::string &knob, int kcap1, int kcap2,
                     const std::string &fill, int fcap1, int fcap2, const std::string &arrow1,
                     const std::string &arrow2, bool has_dis, bool vertical, int blend, int alpha, int ck, int ck2,
                     int z) {
	(void)fcap1, (void)fcap2;
	Engine &e = ui_.engine;
	knob_src_ = load_control_image(e, knob, ck, ck2);
	if (!knob_src_) return false;
	x_ = nx, y_ = ny, z_ = z;
	ox_ = track_dx, oy_ = track_dy;
	vertical_ = vertical;
	has_dis_ = has_dis;
	blend_ = blend, alpha_ = alpha;
	kcap1_ = kcap1, kcap2_ = kcap2;
	knob_frames_ = 3 + (has_dis ? 1 : 0);
	knob_thick_ = (vertical ? knob_src_->width() : knob_src_->height()) / knob_frames_;
	knob_len_ = vertical ? knob_src_->height() : knob_src_->width();
	knob_img_ = knob_src_;
	Rect r = vertical ? Rect(0, 0, knob_thick_, knob_len_) : Rect(0, 0, knob_len_, knob_thick_);
	knob_ = std::make_unique<Sprite>(knob_img_, &r, 0, 0);
	knob_->set_mode(blend);
	knob_->set_opacity(alpha);
	if (!fill.empty()) {
		fill_img_ = load_control_image(e, fill, ck, ck2);
		if (fill_img_) {
			fill_ = std::make_unique<Sprite>(fill_img_, nullptr, 0, 0);
			fill_->set_mode(blend);
			fill_->set_opacity(alpha);
			ui_.attach(fill_.get(), z);
		}
	}
	ui_.attach(knob_.get(), z);
	if (!arrow1.empty()) {
		arrow1_ = std::make_unique<ArrowButton>(ui_, this, vertical ? 0 : 2);
		if (!arrow1_->init_file(arrow1, has_dis, true, 0, 0, blend, alpha, ck, ck2, z)) arrow1_.reset();
		else if (vertical) oy_ += arrow1_->height();
		else ox_ += arrow1_->width();
	}
	if (!arrow2.empty()) {
		arrow2_ = std::make_unique<ArrowButton>(ui_, this, vertical ? 1 : 3);
		if (!arrow2_->init_file(arrow2, has_dis, true, 0, 0, blend, alpha, ck, ck2, z)) arrow2_.reset();
	}
	track_len_ = vertical ? track_dy : track_dx;
	value_ = -1;
	layout();
	return true;
}

// 3-slice knob: caps kept, middle stretched along the axis; frames stay
// stacked perpendicular to the bar.
ImageRef ScrollBar::build_knob(int len) {
	const Image &src = *knob_src_;
	int slen = vertical_ ? src.height() : src.width();
	int mid_src = slen - kcap1_ - kcap2_;
	int mid = len - kcap1_ - kcap2_;
	if (mid_src <= 0 || mid <= 0) return knob_src_;
	auto img = std::make_shared<Image>(vertical_ ? src.width() : len, vertical_ ? len : src.height(), 32);
	img->has_alpha = src.has_alpha;
	img->color_key = src.color_key;
	if (vertical_) {
		copy_rect(*img, 0, 0, src, Rect(0, 0, src.width(), kcap1_));
		stretch_blit(*img, src, 0, kcap1_, src.width(), mid, Rect(0, kcap1_, src.width(), kcap1_ + mid_src), 0, 0,
		             255, false);
		copy_rect(*img, 0, kcap1_ + mid, src, Rect(0, slen - kcap2_, src.width(), slen));
	} else {
		copy_rect(*img, 0, 0, src, Rect(0, 0, kcap1_, src.height()));
		stretch_blit(*img, src, kcap1_, 0, mid, src.height(), Rect(kcap1_, 0, kcap1_ + mid_src, src.height()), 0, 0,
		             255, false);
		copy_rect(*img, kcap1_ + mid, 0, src, Rect(slen - kcap2_, 0, slen, src.height()));
	}
	return img;
}

int ScrollBar::set_range(int length, int total, int page, int v, int p5, int min_knob) {
	(void)p5;
	int a1 = arrow1_ ? (vertical_ ? arrow1_->height() : arrow1_->width()) : 0;
	int a2 = arrow2_ ? (vertical_ ? arrow2_->height() : arrow2_->width()) : 0;
	track_len_ = length - a1 - a2;
	if (track_len_ < 0) track_len_ = 0;
	if (kcap1_ || kcap2_) {
		int k = total > 0 ? int(int64_t(track_len_) * page / total) : track_len_;
		k = std::max({k, min_knob, kcap1_ + kcap2_ + 1});
		if (k > track_len_) k = track_len_;
		knob_len_ = k;
		knob_img_ = build_knob(k);
		knob_->set_image_keep(knob_img_);
	} else {
		knob_len_ = vertical_ ? knob_src_->height() : knob_src_->width();
	}
	max_ = total - page;
	if (max_ < 0) max_ = 0;
	range_ = track_len_ - knob_len_;
	if (range_ < 0) range_ = 0;
	value_ = std::max(0, std::min(v, max_));
	knob_px_ = px_of(value_);
	set_knob_state(knob_state_);
	layout();
	set_enabled(true);
	return 0;
}

void ScrollBar::layout() {
	int tx = x_ + ox_, ty = y_ + oy_;
	if (knob_) knob_->set_pos(vertical_ ? tx : tx + knob_px_, vertical_ ? ty + knob_px_ : ty);
	if (fill_ && fill_img_) {
		int len = knob_px_ + knob_len_ / 2;
		fill_->src = vertical_ ? Rect(0, 0, fill_img_->width(), std::min(len, fill_img_->height()))
		                       : Rect(0, 0, std::min(len, fill_img_->width()), fill_img_->height());
		fill_->set_pos(tx, ty);
	}
	if (arrow1_) {
		int a = vertical_ ? arrow1_->height() : arrow1_->width();
		arrow1_->set_pos(vertical_ ? tx : tx - a, vertical_ ? ty - a : ty);
	}
	if (arrow2_) arrow2_->set_pos(vertical_ ? tx : tx + track_len_, vertical_ ? ty + track_len_ : ty);
}

void ScrollBar::set_knob_state(int s) {
	knob_state_ = s;
	int f = s;
	if (!enabled()) f = has_dis_ ? 3 : 0;
	if (f >= knob_frames_) f = 0;
	if (!knob_ || !knob_img_) return;
	int w = vertical_ ? knob_thick_ : knob_img_->width();
	int h = vertical_ ? knob_img_->height() : knob_thick_;
	Rect r = vertical_ ? Rect(w * f, 0, w * (f + 1), h) : Rect(0, h * f, w, h * (f + 1));
	if (!(knob_->src == r)) {
		knob_->src = r;
		knob_->touch();
	}
}

void ScrollBar::notify() {
	if (on_change) on_change(value_);
	else ui_.fire(this);
}

void ScrollBar::set_value(int v) {
	if (v > max_) v = max_;
	if (v < 0) v = 0;
	value_ = v;
	knob_px_ = px_of(v);
	layout();
	notify();
}

void ScrollBar::set_knob_px(int px) {
	if (px > range_) px = range_;
	if (px < 0) px = 0;
	int nv = range_ > 0 ? int(int64_t(max_) * px / range_) : 0;
	if (px == knob_px_ && nv == value_) return;
	knob_px_ = px;
	value_ = nv;
	layout();
	notify();
}

void ScrollBar::step(int d) {
	if (value_ < 0) return;
	int nv = std::max(0, std::min(value_ + d, max_));
	if (nv == value_) return;
	set_value(nv);
}

void ScrollBar::page_step(int dir) {
	set_knob_px(knob_px_ + dir * knob_len_);
}

int ScrollBar::x() const {
	return x_;
}
int ScrollBar::y() const {
	return y_;
}
int ScrollBar::width() const {
	int a2 = arrow2_ ? arrow2_->width() : 0;
	return vertical_ ? ox_ + knob_thick_ : ox_ + track_len_ + a2;
}
int ScrollBar::height() const {
	int a2 = arrow2_ ? arrow2_->height() : 0;
	return vertical_ ? oy_ + track_len_ + a2 : oy_ + knob_thick_;
}

void ScrollBar::set_pos(int nx, int ny) {
	x_ = nx, y_ = ny;
	layout();
}

void ScrollBar::set_visible(bool v) {
	if (!v) ui_.on_hidden(this);
	visible_ = v;
	if (knob_) knob_->set_visible(v);
	if (fill_) fill_->set_visible(v);
	if (arrow1_) arrow1_->set_visible(v);
	if (arrow2_) arrow2_->set_visible(v);
}

void ScrollBar::set_z(int z) {
	z_ = z;
	for (Layer *l : {static_cast<Layer *>(knob_.get()), static_cast<Layer *>(fill_.get())})
		if (l && l->list()) l->list()->reprioritize(l, z, true);
	if (arrow1_) arrow1_->set_z(z);
	if (arrow2_) arrow2_->set_z(z);
}

bool ScrollBar::set_enabled(bool e) {
	if (arrow1_) arrow1_->set_enabled(e);
	if (arrow2_) arrow2_->set_enabled(e);
	if (!UiControl::set_enabled(e)) return false;
	set_knob_state(0);
	return true;
}

// FUN_004cada0: the track rectangle (the fill sprite when there is one).
bool ScrollBar::hit_test(int sx, int sy) const {
	if (fill_ && !rect_hit_) return fill_->hit(sx, sy, pixel_hit_possible(*fill_img_));
	int tx = x_ + ox_, ty = y_ + oy_;
	int w = vertical_ ? knob_thick_ : track_len_, h = vertical_ ? track_len_ : knob_thick_;
	return sx >= tx && sy >= ty && sx < tx + w && sy < ty + h;
}

void ScrollBar::on_mouse_enter() {
	if (!dragging_) set_knob_state(1);
}

void ScrollBar::on_mouse_leave() {
	if (dragging_ || paging_) {
		dragging_ = false;
		paging_ = 0;
		ui_.set_capture(false);
	}
	set_knob_state(0);
}

void ScrollBar::on_mouse_move(int lx, int ly) {
	if (dragging_) set_knob_px(drag_px0_ + axis(lx, ly) - anchor_);
}

// FUN_004d4300
bool ScrollBar::on_mouse(int lx, int ly) {
	int a = axis(lx, ly);
	if (value_ < 0) return false;
	if (ui_.mouse_pressed(3) || ui_.mouse_pressed(4)) {
		int d = ui_.mouse_pressed(3) ? -1 : 1;
		if (wheel_step_ > 0) set_value(value_ + d * wheel_step_);
		else if (wheel_step_ < 0) set_knob_px(knob_px_ + d * -wheel_step_);
		else step(d);
		return true;
	}
	if (ui_.mouse_pressed(0)) {
		if (a < knob_px_) {
			paging_ = -1;
		} else if (a >= knob_px_ + knob_len_) {
			paging_ = 1;
		} else {
			dragging_ = true;
			anchor_ = a;
			drag_px0_ = knob_px_;
			ui_.set_capture(true);
			set_knob_state(2);
			return true;
		}
		page_step(paging_);
		repeat_at_ = ui_.now() + 500;
		return true;
	}
	if (ui_.mouse_held(0)) {
		if (dragging_) {
			set_knob_px(drag_px0_ + a - anchor_);
			return true;
		}
		if (paging_) {
			if (a >= knob_px_ && a < knob_px_ + knob_len_) {
				paging_ = 0;
			} else if (ui_.now() >= repeat_at_) {
				page_step(paging_);
				repeat_at_ = ui_.now() + 100;
			}
			return true;
		}
		return false;
	}
	if (dragging_ || paging_) {
		dragging_ = false;
		paging_ = 0;
		ui_.set_capture(false);
		bool on_knob = a >= knob_px_ && a < knob_px_ + knob_len_ && hit_test(x_ + lx, y_ + ly);
		set_knob_state(on_knob || hit_test(x_ + lx, y_ + ly) ? 1 : 0);
		return true;
	}
	return false;
}

bool ScrollBar::on_key() {
	if (value_ < 0) return false;
	int d = 0;
	if (vertical_) d = ui_.action_pressed(0) ? -1 : ui_.action_pressed(1) ? 1 : 0;
	else d = ui_.action_pressed(2) ? -1 : ui_.action_pressed(3) ? 1 : 0;
	if (!d) return false;
	step(d);
	return true;
}

void ScrollBar::on_set_focus(UiControl *, int) {
	set_knob_state(1);
	ui_.park_mouse();
}

void ScrollBar::on_kill_focus(UiControl *) {
	if (!dragging_) set_knob_state(0);
}

// vt+0x9c: [arrow1, bar, arrow2] at pos, pos+1, pos+2.
int ScrollBar::group_add(int pos) {
	if (in_group) return 2;
	int p = pos;
	if (arrow1_ && ui_.insert_node(arrow1_.get(), p) == 0 && p >= 0) ++p;
	int r = ui_.insert_node(this, p);
	if (p >= 0) ++p;
	if (arrow2_) ui_.insert_node(arrow2_.get(), p);
	return r;
}

void ScrollBar::group_remove() {
	if (arrow1_) ui_.erase_node(arrow1_.get());
	if (arrow2_) ui_.erase_node(arrow2_.get());
	ui_.erase_node(this);
}

// ---------------------------------------------------------------- ListBox (§6.5)

ListBox::~ListBox() = default;

bool ListBox::init(Font *font, int height, int width, int nx, int ny, const std::string &knob, int cap1, int cap2,
                   int sb_length, int sb_x, int sb_y, int blend, const std::string &marker, int spacing, int z,
                   uint32_t bg, uint32_t text_color, uint32_t sel_color, bool marker_flag) {
	if (!font || width < 1 || knob.empty() || sb_length < cap1 + 1 + cap2) return false;
	Engine &e = ui_.engine;
	font_ = font->handle();
	bg_ = bg, text_color_ = text_color, sel_color_ = sel_color;
	marker_flag_ = marker_flag ? 1 : 0;
	if (!marker.empty()) {
		marker_img_ = e.load_image(marker, false);
		if (!marker_img_) return false;
		marker_w_ = marker_img_->width() / 4;
		width += marker_w_;
	}
	font_h_ = std::abs(font->size);
	spacing_ = spacing;
	pitch_ = font_h_ + spacing_;
	if (height < 0) height = -((font_h_ + spacing_) * height + spacing_);
	if (height <= 0) return false;
	body_img_ = std::make_shared<Image>(width, height, 32);
	body_img_->has_alpha = true;
	fill_rect(*body_img_, body_img_->rect(), bg_);
	body_ = std::make_unique<Sprite>(body_img_, nullptr, nx, ny);
	body_->set_mode(kBlendAlpha);
	sb_ = std::make_unique<ScrollBar>(ui_);
	if (!sb_->init(sb_x, sb_y, 0, 0, knob, cap1, cap2, "", 0, 0, "", "", true, true, blend, 255, -1, -1, z)) {
		sb_.reset();
		return false;
	}
	sb_->on_change = [this](int) { redraw(); };
	sb_->set_wheel_step(pitch_);  // one row per wheel notch (the original's step is unknown)
	sb_length_ = sb_length;
	ui_.attach(body_.get(), z);
	return true;
}

int ListBox::add_item(const char *c1, const char *c2, bool redraw_now) {
	if (!lists_made_) {
		lists_made_ = true;
		has_col1_ = c1 != nullptr;
		has_col2_ = c2 != nullptr;
	}
	if (has_col1_) {
		col1_.push_back(c1 ? c1 : "");
		if (marker_img_) markers_.push_back(marker_flag_);
	}
	if (has_col2_) col2_.push_back(c2 ? c2 : "");
	if (redraw_now) relayout();
	return 0;
}

// FUN_004988e0
void ListBox::relayout() {
	int n = count();
	int total = n > 0 ? pitch_ * n - spacing_ : height();
	if (sb_) sb_->set_range(sb_length_, total, height(), scroll(), 0, 0);
	hover_ = -1;
	redraw();
}

// FUN_00498d50
void ListBox::redraw() {
	if (!body_img_) return;
	Image &img = *body_img_;
	fill_rect(img, img.rect(), bg_);
	Font *font = Object::as<Font>(font_);
	const std::vector<std::string> &text = has_col1_ ? col1_ : col2_;
	int n = int(text.size());
	if (!font || n == 0) {
		if (body_) body_->touch();
		return;
	}
	int x0 = marker_img_ ? marker_w_ : 0;
	int w = img.width() - x0;
	int mh = marker_img_ ? marker_img_->height() : 0;
	std::string face = font->face;
	bool face_changed = false;
	int y = -scroll();
	for (int i = 0; i < n && y < img.height(); ++i, y += pitch_) {
		if (y <= -font_h_) continue;
		if (i == selected_) fill_rect(img, Rect(x0, y, x0 + w, y + font_h_), sel_color_);
		if (marker_img_ && size_t(i) < markers_.size()) {
			int f = markers_[i] ? 2 : 0;
			if (hover_ - n == i) f += 1;
			blit(img, *marker_img_, 0, y + ((font_h_ - mh) >> 1), Rect(f * marker_w_, 0, (f + 1) * marker_w_, mh),
			     kBlendCopy, 255, false);
		}
		if (i == hover_) {
			uint32_t c = text_color_;
			fill_rect(img, Rect(x0, y, x0 + w, y + 1), c);
			fill_rect(img, Rect(x0, y + 1, x0 + 1, y + font_h_), c);
			fill_rect(img, Rect(x0 + 1, y + font_h_ - 1, x0 + w, y + font_h_), c);
			fill_rect(img, Rect(x0 + w - 1, y + 1, x0 + w, y + font_h_ - 1), c);
		}
		// the second column names the face each row is drawn with (font list)
		if (has_col2_ && size_t(i) < col2_.size() && font->face != col2_[i]) {
			font->face = col2_[i];
			font->invalidate_cache();
			face_changed = true;
		}
		font->draw(img, x0 + 2, y, text[i], true);
	}
	if (face_changed) {
		font->face = face;
		font->invalidate_cache();
	}
	if (body_) body_->touch();
}

int ListBox::set_marker(int i, int flag, bool redraw_now) {
	if (!marker_img_ || !has_col1_) return 1;
	if (i < 0 || size_t(i) >= markers_.size()) return 2;
	markers_[i] = uint8_t(flag);
	if (redraw_now) redraw();
	return 0;
}

int ListBox::marker(int i) const {
	if (i < 0 || size_t(i) >= markers_.size()) return 0;
	return markers_[i];
}

void ListBox::set_hover(int i) {
	if (i == hover_) return;
	hover_ = i;
	redraw();
}

// FUN_00499490: item → select + fire; count + i → toggle marker i + fire.
void ListBox::select(int i) {
	int n = count();
	if (n == 0) return;
	if (marker_img_ && has_col1_ && i >= n) {
		int k = i - n;
		if (k >= n) return;
		markers_[k] = markers_[k] ? 0 : 1;
	} else {
		if (i < 0 || i >= n || i == selected_) return;
		selected_ = i;
	}
	ui_.fire(this);
	redraw();
}

// FUN_004995b0
bool ListBox::scroll_into_view(int i) {
	int s = scroll();
	int top = pitch_ * i - s;
	int target = -1;
	if (top < 0) target = top + s;
	else if (top + font_h_ > height()) target = top - height() + s + font_h_;
	if (target >= 0 && sb_) sb_->set_value(target);  // redraws through on_change
	else redraw();
	return true;
}

// FUN_00498b80
void ListBox::set_value_int(int v) {
	int n = count();
	if (v >= n) v = -1;
	selected_ = v;
	if (v >= 0) scroll_into_view(v);
	else redraw();
}

void ListBox::on_mouse_move(int lx, int ly) {
	int n = count();
	int i = -1;
	if (ly >= 0 && pitch_ > 0) {
		i = (ly + scroll()) / pitch_;
		if (i >= n) i = -1;
		else if (marker_img_ && has_col1_ && lx < marker_w_) i += n;
	}
	set_hover(i);
}

void ListBox::on_mouse_leave() {
	set_hover(-1);
}

// FUN_00499130
bool ListBox::on_mouse(int lx, int ly) {
	if (ui_.mouse_pressed(0)) {
		select(hover_);
		return true;
	}
	if ((ui_.mouse_pressed(3) || ui_.mouse_pressed(4)) && sb_) return sb_->on_mouse(lx, ly);
	return false;
}

// FUN_004991e0
bool ListBox::on_key() {
	int n = count();
	if (n == 0) return false;
	if (ui_.action_pressed(0)) {
		if (hover_ >= n) hover_ -= n;
		hover_ = std::max(0, hover_ - 1);
		scroll_into_view(hover_);
		return true;
	}
	if (ui_.action_pressed(1)) {
		if (hover_ >= n) hover_ -= n;
		if (hover_ < n - 1) ++hover_;
		scroll_into_view(hover_);
		return true;
	}
	if (ui_.action_pressed(4)) {
		select(hover_);
		return true;
	}
	if (ui_.key_pressed(0xC7)) {  // Home
		hover_ = 0;
		if (sb_) sb_->set_value(0);
		redraw();
		return true;
	}
	if (ui_.key_pressed(0xCF)) {  // End
		hover_ = n - 1;
		if (sb_) sb_->set_value(sb_->max_value());
		redraw();
		return true;
	}
	if (ui_.key_pressed(0x39) && marker_img_ && has_col1_ && hover_ >= 0) {  // Space: marker
		select(hover_ < n ? hover_ + n : hover_);
		return true;
	}
	return false;
}

void ListBox::set_pos(int nx, int ny) {
	if (!body_) return;
	int dx = nx - body_->x, dy = ny - body_->y;
	body_->set_pos(nx, ny);
	if (sb_) sb_->set_pos(sb_->x() + dx, sb_->y() + dy);
}

void ListBox::set_visible(bool v) {
	if (sb_) sb_->set_visible(v);
	if (!v) ui_.on_hidden(this);
	else if (ui_.hover() == this) ui_.drop_hover();
	if (body_) body_->set_visible(v);
}

void ListBox::set_z(int z) {
	if (body_ && body_->list()) body_->list()->reprioritize(body_.get(), z, true);
	if (sb_) sb_->set_z(z);
}

bool ListBox::set_enabled(bool e) {
	if (sb_) sb_->set_enabled(e);
	return UiControl::set_enabled(e);
}

// FUN_00498830: the list, then its scroll bar.
int ListBox::group_add(int pos) {
	if (in_group) return -1;
	if (pos < 0) {
		ui_.insert_node(this, -1);
		if (sb_) sb_->group_add(-1);
	} else {
		if (sb_) sb_->group_add(-1);
		ui_.insert_node(this, pos);
	}
	return 0;
}

void ListBox::group_remove() {
	if (sb_) sb_->group_remove();
	ui_.erase_node(this);
}

// ---------------------------------------------------------------- HitRegion (§6.6)

void HitRegion::set_visible(bool v) {
	if (!v) ui_.on_hidden(this);
	visible_ = v;
}

bool HitRegion::hit_test(int sx, int sy) const {
	return active_ && sx >= x_ && sy >= y_ && sx < x_ + w_ && sy < y_ + h_;
}

void HitRegion::on_mouse_move(int, int) {
	if (!hovered_) {
		hovered_ = true;
		ui_.play_se(-3);
	}
}

// FUN_004cb2b0: fires on the press edge.
bool HitRegion::on_mouse(int, int) {
	if (hovered_ && ui_.mouse_pressed(0)) {
		ui_.fire(this);
		ui_.play_se(-4);
		return true;
	}
	return false;
}

void HitRegion::on_set_focus(UiControl *, int) {
	if (!hovered_) {
		hovered_ = true;
		ui_.play_se(-3);
	}
	ui_.park_mouse();
}

bool HitRegion::on_key() {
	if (hovered_ && ui_.action_pressed(4)) {
		ui_.fire(this);
		ui_.play_se(-4);
		return true;
	}
	return false;
}

}  // namespace ore
