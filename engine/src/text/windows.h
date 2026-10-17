// ADV windows (docs/re/40_text.md §5-6, docs/re/60_ui.md §6.7): the framed
// text window (type 5) with its layout engine, the message window (0x6f)
// with the reveal controller, and the choice window (type 6).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/scheduler.h"
#include "gfx/layer.h"
#include "ui/ui_system.h"

namespace ore {

class Engine;
class Font;
class MessageManager;
class RevealTask;

// Frame images B/A/G (FUN_00459990 / FUN_0045a170 / FUN_00459ff0): A and G
// are grayscale alpha masks; mode 1 = colour through A with B drawn on top,
// mode 0 = colour through G (no B). Alpha scales the mask.
struct FrameSet {
	ImageRef b, a, g;
	ImageRef out;  // composed frame (+0x240)
	bool load(Engine &e, const char *pb, const char *pa, const char *pg, int frames = 1);
	bool any() const { return b || a || g; }
	void compose(int mode, uint32_t rgb, int alpha);
};

class TextWindow : public UiControl {
public:
	TextWindow(Engine &e, UiSystem &ui);
	~TextWindow() override;

	int ui_type() const override { return 5; }
	// N_2b1: vt+0x80 (frames) + vt+0x130 (init). 0 ok.
	int create(int x, int y, int w, int h, uint32_t font, uint32_t ruby, const char *b, const char *a, const char *g,
	           int z);

	// UiControl
	int x() const override { return sprite_ ? sprite_->x : 0; }
	int y() const override { return sprite_ ? sprite_->y : 0; }
	int width() const override { return box_w; }
	int height() const override { return box_h; }
	void set_pos(int nx, int ny) override;
	void set_visible(bool v) override;
	bool visible() const override { return sprite_ && sprite_->visible; }
	void set_z(int z) override;
	Layer *sprite(int i) override { return i == 0 ? sprite_.get() : nullptr; }
	ImageRef image(int i) override { return i == 0 ? surface : nullptr; }
	void set_value_str(const char *s) override;  // N_2d9: append text
	void set_active_mode(bool b) override { active_mode = b; }

	Font *font() const;
	Font *ruby_font() const;

	// --- vtable methods of the text object
	void set_bg_style(int mode, uint32_t rgb, int alpha);           // vt+0x84
	void set_margins(int l, int t, int r, int b, bool redraw);      // vt+0x88
	void set_pads(int l, int t, int r, int b, bool redraw);         // vt+0x8c
	void relayout();          // vt+0xf0: line metrics
	void update_wrap();       // vt+0x100
	virtual void clear_page();  // vt+0xa8
	void redraw_background();   // vt+0xa4
	bool newline();             // vt+0xb0: true = page full
	int put_char(uint16_t code);  // vt+0xb4: 0 drawn, -1 drawn after wrap, 1 page full
	void draw_string(const std::string &s, int x, int y);  // vt+0xb8 (-1 = current)
	void print_aligned(const std::string &s, int mode);     // vt+0x118

	Engine &engine;
	uint32_t font_h = 0, ruby_h = 0;
	int box_w = 0, box_h = 0;
	uint32_t bg_rgb = 0;
	int bg_alpha = 0x80, bg_mode = 1;
	int ml = 8, mt = 8, mr = 8, mb = 8;
	int min_line_h = 0, line_h = 0, line_adv = 0;
	int pl = 2, pt = 2, pr = 2, pb = 2;
	int cx = 0, cy = 0;
	bool fullwidth = false, autowrap = true, active_mode = false;
	int line_spacing = 0, char_spacing = 0, ruby_gap = -2, wrap_reserve = 2, wrap_x = 0;
	int indent = 0;
	uint16_t prev_char = 0;
	bool next_overflow = false;
	int z = 0;
	FrameSet frame;
	ImageRef surface;  // +0xfc

protected:
	void touch();
	std::unique_ptr<Sprite> sprite_;
};

class MessageWindow : public TextWindow {
public:
	MessageWindow(Engine &e, UiSystem &ui, MessageManager &mm);
	~MessageWindow() override;

	int ui_type() const override { return 0x6f; }
	void set_pos(int nx, int ny) override;  // moves the name box too
	void clear_page() override;             // also hides the name box

	int show(bool allow_skip);  // N_2fa
	int show_instant();         // N_2fb
	int control(int mode);      // N_2fc
	int state() const;          // N_300
	int skip_flag() const;      // N_301
	void set_cursor_visible(bool v);
	void set_cursor(uint32_t anim);  // N_2fd
	bool advance_pressed();     // FUN_004093c0

	MessageManager &mm;
	uint32_t name_box = 0;   // +0x2c0
	uint32_t skip_button = 0;  // +0x2c4
	uint32_t cursor = 0;       // +0x2bc: N_20d FrameAnim drawn into this window
	uint32_t cursor_task = 0;  // +0x2a4: FrameAnimTask playing it (paused while hidden)
	int name_mode = 0;         // +0x2c8
	bool cursor_visible = false, has_name = false;
	bool click_completes = true;  // +0x2a8
	std::string voice_id;         // +0x2a9
	RevealTask *ctrl = nullptr;   // +0x2a0

private:
	int start(bool instant, bool allow_skip);  // FUN_00409570
	friend class RevealTask;
};

// FUN_00409060 class (vtable 0x4ed924): per-frame reveal of the current message.
class RevealTask : public Task {
public:
	static constexpr int kType = 0x6f6f;
	explicit RevealTask(MessageWindow *w) : Task(kType), win(w) {}
	bool update(int64_t now) override;  // FUN_00409450
	void finish() override { win = nullptr; }
	int reveal(bool all);  // FUN_00409090
	int reveal_page();     // FUN_00409240
	void leave_wait();     // FUN_00409370
	void restore_size();

	MessageWindow *win;
	int st = 0;            // +0x24
	int last = 0;          // +0x25
	bool stopped = true;   // +0x26 (task paused)
	bool read = false;     // +0x27
	bool auto_skip = false;  // +0x28
	int ms = 0;            // +0x29
	bool no_typewriter = false;  // +0x2a
	int64_t next_at = 0;   // +0x2c
	int saved_size = 0;    // +0x30
};

class SelectWindow : public UiControl {
public:
	SelectWindow(Engine &e, UiSystem &ui);
	~SelectWindow() override;

	int ui_type() const override { return 6; }
	int create(int x, int y, int w, int h, int spacing, uint32_t font, int z);  // N_2b3
	// N_304
	int set_images(int frames, bool pressed, bool disabled_frame, const char *b1, const char *a1, const char *g1,
	               const char *b0, const char *a0, const char *g0, const char *b2, const char *a2, const char *g2);
	int add_item(const char *text, bool disabled);  // N_309
	void clear();                                   // N_2ea
	int highlight(int i, bool on);                  // N_30a
	bool item_pos(int i, int &px, int &py);         // N_2eb
	void set_bg_style(int mode, uint32_t rgb, int alpha);  // vt+0x88
	void layout();

	// UiControl
	int x() const override { return x_; }
	int y() const override { return y_; }
	int width() const override;
	int height() const override;
	void set_pos(int nx, int ny) override;
	void set_visible(bool v) override;
	bool visible() const override { return visible_; }
	void set_z(int nz) override;
	bool hit_test(int sx, int sy) const override;
	void on_mouse_move(int lx, int ly) override;
	void on_mouse_leave() override;
	bool on_mouse(int lx, int ly) override;
	bool on_key() override;
	Layer *sprite(int i) override;
	ImageRef image(int i) override;
	void set_active_mode(bool b) override { static_mode = b; }
	int32_t value() override { return hover; }

	Engine &engine;
	uint32_t font_h = 0;
	int spacing = 0;
	uint32_t col_normal = 0xffffff, col_chosen = 0xffffff, col_hover = 0xffffff, col_disabled = 0x909090;
	uint32_t grad[8] = {0, 0, 0, 0, 0, 0, 0xb0b0b0, 0x595959};  // normal, chosen, hover, disabled pairs
	int hl_mode = 0;
	uint32_t hl_color = 0;
	int ml = 0, mt = 0, mr = 0, mb = 0, tx = 0, ty = 0, press_dx = 0, press_dy = 0;
	int bg_mode = 1, bg_alpha = 0x80;
	uint32_t bg_rgb = 0;
	bool mark_chosen = true;  // +0x40
	bool static_mode = false;  // +0xe9
	int frames = 1;
	bool has_pressed = false, has_disabled = false;

private:
	struct Item {
		ImageRef img;
		std::unique_ptr<Sprite> spr;
		int fw = 0, fh = 0, nframes = 2;
		bool disabled = false;
		int frame = 0;
	};
	void set_frame(Item &it, int f);
	int item_at(int lx, int ly) const;
	void attach(Sprite *s);
	void refresh_frames();
	FrameSet item_bg_, top_, bottom_;
	std::unique_ptr<Sprite> top_spr_, bottom_spr_;
	std::vector<std::unique_ptr<Item>> items_;
	int x_ = 0, y_ = 0, w_ = 0, h_ = 0, z_ = 0;
	bool visible_ = true;
	int hover = -1, pressed_ = -1, focus_ = -1;
	uint32_t page_handle_ = 0;
	bool page_list_b_ = false;
};

}  // namespace ore
