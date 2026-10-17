// UI controls (docs/re/60_ui.md §6): Button (0), CheckButton (1),
// EditBox (3), ScrollBar (4), HitRegion (7), ListBox (9).
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/ui_system.h"

namespace ore {

class Font;

// A control drawn by one sprite showing a horizontal frame of its image.
class SpriteControl : public UiControl {
public:
	explicit SpriteControl(UiSystem &ui) : UiControl(ui) {}
	~SpriteControl() override;

	int x() const override { return sprite_ ? sprite_->x : 0; }
	int y() const override { return sprite_ ? sprite_->y : 0; }
	int width() const override { return fw_; }
	int height() const override { return fh_; }
	void set_pos(int nx, int ny) override;
	void set_visible(bool v) override;
	bool visible() const override { return sprite_ && sprite_->visible; }
	void set_z(int z) override;
	bool hit_test(int sx, int sy) const override;
	Layer *sprite(int) override { return sprite_.get(); }
	ImageRef image(int) override { return image_; }
	void set_rect_hit(bool b) override { rect_hit_ = b; }

protected:
	// Creates the sprite (frame 0, frame size fw x fh) and attaches it to the
	// factory's parent list at z.
	bool init_sprite(ImageRef img, int frames, int nx, int ny, int z, int blend, int alpha);
	void show_frame(int f);

	ImageRef image_;
	std::unique_ptr<Sprite> sprite_;
	int fw_ = 0, fh_ = 0, frames_ = 1, z_ = 0;
	bool rect_hit_ = false;
};

// Loads an ELG for a control and applies the colour-key arguments
// (ck2 >= 0: key = pixel (ck, ck2); else ck != -1: key = ck).
ImageRef load_control_image(Engine &e, const std::string &path, int ck, int ck2);

class Button : public SpriteControl {
public:
	explicit Button(UiSystem &ui) : SpriteControl(ui) {}
	int ui_type() const override { return 0; }

	// vt+0x9c FUN_004c7ab0 / vt+0x98 FUN_004c7c70
	bool init_file(const std::string &path, bool has_dis, bool has_pressed, int nx, int ny, int blend, int alpha,
	               int ck, int ck2, int z);
	bool init_image(ImageRef img, bool has_dis, bool has_pressed, int nx, int ny, int blend, int alpha, int z);

	bool set_enabled(bool e) override;
	void on_mouse_move(int lx, int ly) override;
	void on_mouse_leave() override;
	bool on_mouse(int lx, int ly) override;
	void on_set_focus(UiControl *prev, int dir) override;
	void on_kill_focus(UiControl *next) override;
	bool on_key() override;
	int state() const { return state_; }

protected:
	void set_state(int s);  // FUN_004c7ed0
	virtual void do_fire();  // FUN_004d9740

	bool has_dis_ = false, has_pressed_ = false;
	int state_ = 0;  // 0 normal, 1 hover, 2 pressed, 3 disabled
	int64_t key_timer_ = 0;
};

class CheckButton : public SpriteControl {
public:
	explicit CheckButton(UiSystem &ui) : SpriteControl(ui) {}
	~CheckButton() override;
	int ui_type() const override { return 1; }

	// vt+0xb8 / vt+0xbc: style 0..3 (bit 0: no pressed frame, bit 1: radio)
	bool init_image(ImageRef img, int style, bool has_dis, CheckButton *leader, int nx, int ny, int blend, int alpha,
	                int z);
	bool init_file(const std::string &path, int style, bool has_dis, CheckButton *leader, int nx, int ny, int blend,
	               int alpha, int ck, int ck2, int z);

	bool checked() const { return state_ >= 3 && state_ != 6; }
	bool radio() const { return (style_ & 2) != 0; }
	void set_checked(bool c);  // vt+0x8c (N_2d9)
	void select_index(int i);  // vt+0x90 (N_2de)
	int selected_index() const;  // N_2df

	bool set_enabled(bool e) override;
	void on_mouse_move(int lx, int ly) override;
	void on_mouse_leave() override;
	bool on_mouse(int lx, int ly) override;
	void on_set_focus(UiControl *prev, int dir) override;
	void on_kill_focus(UiControl *next) override;
	bool on_key() override;
	void set_value_int(int v) override { set_checked(v != 0); }
	int32_t value() override { return checked() ? 1 : 0; }
	void set_active_mode(bool b) override { inactive_ = b; }

private:
	void set_state(int s);
	void toggle();  // FUN_004da920
	void fire();
	int base() const { return checked() ? 3 : 0; }
	void uncheck_others(CheckButton *keep);

	int style_ = 0;
	bool has_dis_ = false;
	int state_ = 0;
	bool inactive_ = false;
	int64_t key_timer_ = 0;
	CheckButton *leader_ = nullptr;           // members: their leader
	std::vector<CheckButton *> members_;       // leader: all radios incl. itself
	int selected_ = 0;                         // leader: checked member index
};

class EditBox : public SpriteControl {
public:
	explicit EditBox(UiSystem &ui) : SpriteControl(ui) {}
	~EditBox() override;
	int ui_type() const override { return 3; }

	// vt+0xb0 FUN_004c60c0
	bool init(Font *font, int lines, int width, int nx, int ny, int padding, int spacing, int border, int z,
	          uint32_t bg_color);

	void set_value_str(const char *s) override;
	int32_t value() override { return int32_t(buffer_); }
	bool on_mouse(int lx, int ly) override;
	void on_set_focus(UiControl *prev, int dir) override;
	void on_kill_focus(UiControl *next) override;
	bool on_text_input() override;
	void set_active_mode(bool b) override { inactive_ = b; }
	const std::string &text() const { return text_; }

private:
	void set_text(const std::string &s);
	void redraw();
	void begin_input();

	uint32_t font_ = 0;
	int lines_ = 0, padding_ = 0, spacing_ = 0, border_ = 0, line_h_ = 0;
	size_t per_line_ = 0, capacity_ = 0;
	uint32_t bg_ = 0;
	std::string text_;
	uint32_t buffer_ = 0;  // VM copy returned by N_2da
	bool focused_ = false, input_pending_ = false, inactive_ = false;
};

class ScrollBar;

// Scroll-bar arrow (Button subclass vtable 0x4f05b8): Fire = owner step.
class ArrowButton : public Button {
public:
	ArrowButton(UiSystem &ui, ScrollBar *owner, int dir) : Button(ui), owner_(owner), dir_(dir) {}

private:
	void do_fire() override;
	ScrollBar *owner_;
	int dir_;  // 0..3: -1 step for 0/2, +1 for 1/3
};

class ScrollBar : public UiControl {
public:
	explicit ScrollBar(UiSystem &ui) : UiControl(ui) {}
	~ScrollBar() override;
	int ui_type() const override { return 4; }

	// vt+0xa4 FUN_004c9d10
	bool init(int nx, int ny, int track_dx, int track_dy, const std::string &knob, int kcap1, int kcap2,
	          const std::string &fill, int fcap1, int fcap2, const std::string &arrow1, const std::string &arrow2,
	          bool has_dis, bool vertical, int blend, int alpha, int ck, int ck2, int z);
	// vt+0x84 FUN_004ca470 (N_2b0)
	int set_range(int length, int total, int page, int value, int p5, int min_knob);
	void set_value(int v);     // vt+0x78
	void set_knob_px(int px);  // vt+0x7c
	void step(int d);          // vt+0x80
	int value_now() const { return value_; }
	int max_value() const { return max_; }
	// Change notification: default = group event (listener DAT_005034c0).
	std::function<void(int)> on_change;
	ArrowButton *arrow(int i) { return i == 0 ? arrow1_.get() : arrow2_.get(); }
	// +0x94: > 0 value units per wheel notch, < 0 pixels, 0 = one step.
	void set_wheel_step(int s) { wheel_step_ = s; }

	int x() const override;
	int y() const override;
	int width() const override;
	int height() const override;
	void set_pos(int nx, int ny) override;
	void set_visible(bool v) override;
	bool visible() const override { return visible_; }
	void set_z(int z) override;
	bool set_enabled(bool e) override;
	bool hit_test(int sx, int sy) const override;
	void on_mouse_move(int lx, int ly) override;
	void on_mouse_enter() override;
	void on_mouse_leave() override;
	bool on_mouse(int lx, int ly) override;
	bool on_key() override;
	void on_set_focus(UiControl *prev, int dir) override;
	void on_kill_focus(UiControl *next) override;
	Layer *sprite(int) override { return knob_.get(); }
	ImageRef image(int) override { return knob_img_; }
	void set_value_int(int v) override { set_value(v); }
	int32_t value() override { return value_; }
	void set_rect_hit(bool b) override { rect_hit_ = b; }
	int group_add(int pos) override;
	void group_remove() override;

private:
	void layout();
	void set_knob_state(int s);
	void notify();
	int axis(int lx, int ly) const { return vertical_ ? ly - oy_ : lx - ox_; }
	int px_of(int v) const { return max_ > 0 ? int(int64_t(range_) * v / max_) : 0; }
	void page_step(int dir);
	ImageRef build_knob(int len);

	int x_ = 0, y_ = 0, z_ = 0;
	int ox_ = 0, oy_ = 0;  // track origin relative to x_, y_ (+0x70/+0x74)
	bool vertical_ = false, visible_ = true, has_dis_ = false, rect_hit_ = false;
	int blend_ = 2, alpha_ = 255;
	int kcap1_ = 0, kcap2_ = 0, knob_frames_ = 3;
	ImageRef knob_src_, knob_img_;
	std::unique_ptr<Sprite> knob_;
	int knob_thick_ = 0;
	ImageRef fill_img_;
	std::unique_ptr<Sprite> fill_;
	std::unique_ptr<ArrowButton> arrow1_, arrow2_;
	int track_len_ = 0, knob_len_ = 0, range_ = 0, knob_px_ = 0;
	int max_ = 0, value_ = -1;
	int knob_state_ = 0;
	bool dragging_ = false;
	int anchor_ = 0, drag_px0_ = 0;
	int paging_ = 0;
	int64_t repeat_at_ = 0;
	int wheel_step_ = 0;  // +0x94
};

class ListBox : public UiControl {
public:
	explicit ListBox(UiSystem &ui) : UiControl(ui) {}
	~ListBox() override;
	int ui_type() const override { return 9; }

	// FUN_00498560
	bool init(Font *font, int height, int width, int nx, int ny, const std::string &knob, int cap1, int cap2,
	          int sb_length, int sb_x, int sb_y, int blend, const std::string &marker, int spacing, int z,
	          uint32_t bg, uint32_t text_color, uint32_t sel_color, bool marker_flag);
	int add_item(const char *col1, const char *col2, bool redraw);  // N_2ec
	void relayout();                                                 // N_2ed(1)
	void redraw();                                                   // N_2ed(0)
	int set_marker(int i, int flag, bool redraw);                    // N_2ee
	int marker(int i) const;                                         // N_2ef
	int hover_index() const { return hover_; }                       // N_2f0

	int x() const override { return body_ ? body_->x : 0; }
	int y() const override { return body_ ? body_->y : 0; }
	int width() const override { return body_img_ ? body_img_->width() : 0; }
	int height() const override { return body_img_ ? body_img_->height() : 0; }
	void set_pos(int nx, int ny) override;
	void set_visible(bool v) override;
	bool visible() const override { return body_ && body_->visible; }
	void set_z(int z) override;
	bool set_enabled(bool e) override;
	void on_mouse_move(int lx, int ly) override;
	void on_mouse_leave() override;
	bool on_mouse(int lx, int ly) override;
	bool on_key() override;
	Layer *sprite(int) override { return body_.get(); }
	ImageRef image(int) override { return body_img_; }
	void set_value_int(int v) override;
	int32_t value() override { return selected_; }
	int group_add(int pos) override;
	void group_remove() override;

private:
	int count() const { return int(std::max(col1_.size(), col2_.size())); }
	void select(int i);  // FUN_00499490
	bool scroll_into_view(int i);
	void set_hover(int i);
	int scroll() const { return sb_ && sb_->value_now() > 0 ? sb_->value_now() : 0; }

	uint32_t font_ = 0;
	ImageRef body_img_, marker_img_;
	std::unique_ptr<Sprite> body_;
	std::unique_ptr<ScrollBar> sb_;
	bool has_col1_ = false, has_col2_ = false, lists_made_ = false;
	std::vector<std::string> col1_, col2_;
	std::vector<uint8_t> markers_;
	int marker_w_ = 0;
	int font_h_ = 0, spacing_ = 0, pitch_ = 0, sb_length_ = 0;
	int selected_ = -1, hover_ = -1;
	uint32_t bg_ = 0, text_color_ = 0, sel_color_ = 0;
	uint8_t marker_flag_ = 1;
};

class HitRegion : public UiControl {
public:
	explicit HitRegion(UiSystem &ui) : UiControl(ui) {}
	int ui_type() const override { return 7; }
	void init(int nx, int ny, int w, int h, bool active) {
		x_ = nx, y_ = ny, w_ = w, h_ = h, active_ = active;
	}
	void set_warp(int wx, int wy) { warp_x_ = wx, warp_y_ = wy; }

	int x() const override { return x_; }
	int y() const override { return y_; }
	int width() const override { return w_; }
	int height() const override { return h_; }
	void set_pos(int nx, int ny) override { x_ = nx, y_ = ny; }
	void set_visible(bool v) override;
	bool visible() const override { return visible_; }
	bool hit_test(int sx, int sy) const override;
	void on_mouse_move(int lx, int ly) override;
	void on_mouse_leave() override { hovered_ = false; }
	bool on_mouse(int lx, int ly) override;
	void on_set_focus(UiControl *prev, int dir) override;
	void on_kill_focus(UiControl *next) override { hovered_ = false; }
	bool on_key() override;

private:
	int x_ = 0, y_ = 0, w_ = 0, h_ = 0, warp_x_ = 0, warp_y_ = 0;
	bool active_ = true, hovered_ = false, visible_ = true;
};

}  // namespace ore
