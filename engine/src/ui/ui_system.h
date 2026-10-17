// UI object system (docs/re/60_ui.md): the control factory (owns every
// control), the control group (hit-test/tab order, hover, focus, event id)
// and the control base class shared by the UI controls (ui/) and the ADV
// windows implemented by the text system (text/).
#pragma once

#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/object.h"
#include "gfx/image.h"
#include "gfx/layer.h"

namespace ore {

class Engine;
class Font;
class FrameAnim;
class ImageObj;
class Surface;
class UiSystem;

// Common control base (vtable 0x4ee3c0). Coordinates are game pixels.
class UiControl : public Object {
public:
	explicit UiControl(UiSystem &ui);
	~UiControl() override;  // unregisters from the group (current and saved lists)

	int ui_type() const override = 0;

	// geometry / visibility
	virtual int x() const = 0;
	virtual int y() const = 0;
	virtual int width() const = 0;
	virtual int height() const = 0;
	virtual void set_pos(int nx, int ny) = 0;
	virtual void set_visible(bool v) = 0;  // hiding drops hover/focus (UiSystem::on_hidden)
	virtual bool visible() const = 0;
	virtual void set_z(int z) { (void)z; }  // N_2d4: re-attach at priority z

	// enable state: returns true when it changed
	virtual bool set_enabled(bool e);
	bool enabled() const { return enabled_; }

	// input (local coordinates for mouse methods)
	virtual bool hit_test(int sx, int sy) const;  // screen coords
	virtual void on_mouse_move(int lx, int ly) { (void)lx, (void)ly; }
	virtual void on_mouse_enter() {}
	virtual void on_mouse_leave() {}
	virtual bool on_mouse(int lx, int ly) { (void)lx, (void)ly; return false; }
	virtual void on_set_focus(UiControl *prev, int dir) { (void)prev, (void)dir; }
	virtual void on_kill_focus(UiControl *next) { (void)next; }
	virtual bool on_key() { return false; }
	// pending typed text delivered to the focused control (Update code 3)
	virtual bool on_text_input() { return false; }
	// per-frame work independent of input (animations, key timers)
	virtual void tick(int64_t now) { (void)now; }

	// script accessors (N_2d7..N_2da); defaults: nothing
	virtual Layer *sprite(int i) { (void)i; return nullptr; }     // N_2d7
	virtual ImageRef image(int i) { (void)i; return nullptr; }    // N_2d8
	virtual void set_value_int(int v) { (void)v; }                // N_2d9 (int forms)
	virtual void set_value_str(const char *s) { (void)s; }        // N_2d9 (string forms: types 3 and 5)
	virtual int32_t value() { return -1; }                        // N_2da
	virtual void set_rect_hit(bool b) { (void)b; }                // N_2db
	virtual void set_active_mode(bool b) { (void)b; }             // N_2dc
	// A layer standing for the control when generic layer natives get it.
	virtual Layer *main_layer() { return sprite(0); }

	// N_2c4 / N_2c5: (un)registration in the group. Composite controls
	// (scroll bar arrows, list box scroll bar) override these to register
	// their parts too; the default handles just this control.
	virtual int group_add(int pos);
	virtual void group_remove();

	// common fields
	std::string tooltip;
	UiControl *neighbour[4] = {nullptr, nullptr, nullptr, nullptr};  // up, down, left, right
	int tag = 0;
	int id = -1;
	bool in_group = false;

	UiSystem &ui() const { return ui_; }

protected:
	UiSystem &ui_;
	bool enabled_ = true;
};

// Where new controls attach: the factory's parent page (N_2b6) list A (main)
// or B (overlay), chosen by N_2c0.
struct UiParent {
	Surface *page = nullptr;
	LayerList *list() const;
};

class UiSystem {
public:
	explicit UiSystem(Engine &e);
	~UiSystem();

	Engine &engine;

	// ---- factory
	UiControl *own(std::unique_ptr<UiControl> c);  // sets c->tag = creation tag
	bool destroy(UiControl *c);
	void destroy_by_tag(int tag, bool invert);
	void destroy_all();
	int creation_tag = 0;    // N_2c1
	bool parent_b = false;   // N_2c0
	Surface *parent_page = nullptr;  // N_2b6
	LayerList *parent_list() const;  // list A or B of the parent page (may be null)
	const std::list<std::unique_ptr<UiControl>> &controls() const { return controls_; }
	bool owns(const UiControl *c) const;
	Surface *parent_surface() const;  // the parent page if it still exists
	// Sets the parent page (N_2b6); also the tooltip page.
	void set_parent(Surface *page);
	// Attaches a layer to the parent list at priority z with the creation tag.
	bool attach(Layer *l, int z);

	// ---- group
	int add_to_group(UiControl *c, int pos);  // N_2c4 result codes
	void remove_from_group(UiControl *c);
	void clear_group();
	int push_group();  // N_2c9
	int pop_group();   // N_2ca
	int update(int64_t now);  // N_2ba
	void reset();              // N_2b9
	int take_event();          // N_2bb
	void set_event(int id) { event_id_ = id; }
	void fire(UiControl *c) {
		if (c && c->id >= 0) event_id_ = c->id;
	}
	bool set_focus(UiControl *c, int dir = 0);
	UiControl *focus() const { return focus_; }
	UiControl *hover() const { return hover_; }
	void on_hidden(UiControl *c);
	void set_capture(bool on) { capture_ = on; }
	void set_active(bool on) { active_ = on; }
	void play_se(int idx);
	// tooltips (N_2b7)
	bool tip_enabled = false;
	int tip_delay = 0, tip_duration = 0;
	uint32_t tip_font = 0;
	Surface *tip_page = nullptr;

	// Input helpers for controls.
	bool action_pressed(int action) const;  // 0 up 1 down 2 left 3 right 4 decide
	bool mouse_pressed(int b) const;
	bool mouse_held(int b) const;
	int64_t now() const;

	// ---- extensions
	// Node-level group operations used by group_add/group_remove.
	int insert_node(UiControl *c, int pos);  // 0 ok, 1 null, 2 already registered
	void erase_node(UiControl *c);           // from the current or a saved list
	// Status form of set_focus (FUN_00448e30): 0 ok, 1 empty list, 2 not
	// registered, 3 disabled/hidden.
	int focus_control(UiControl *c, int dir = 1);
	// Called by ~UiControl: forget every reference to c.
	void forget(UiControl *c);
	// Keyboard-focus mouse policy (group vt+0x44 = 2): park the cursor.
	void park_mouse();
	// Drops the mouse hover (FUN_004496a0): hover->on_mouse_leave(), hide tooltip.
	bool drop_hover();
	bool key_pressed(int dik) const;
	bool key_held(int dik) const;
	int cursor_shape = 0;   // N_2b8
	int tip_offset_y = 0x16;
	int last_result = 0;
	// Script handle for an image owned by a control (N_2d8): a cached ImageObj.
	uint32_t image_handle(const ImageRef &img);
	// Mouse position as seen by the last update (-1 after reset).
	int mouse_x() const { return mouse_x_; }
	int mouse_y() const { return mouse_y_; }
	// Tooltip display (FUN_00408a20 / FUN_004087b0).
	void show_tooltip(const std::string &text, int mx, int my);
	void hide_tooltip();
	const std::vector<UiControl *> &group() const { return group_; }
	// Frame-animation sources created by N_20d (list DAT_005034d0).
	std::vector<std::unique_ptr<FrameAnim>> frame_anims;

private:
	void drop_hover_focus();
	void set_focus_node(UiControl *c, int dir);
	int focus_next();
	int focus_prev();
	int tab_direction() const;
	bool apply_tab(UiControl *f, int dir);
	void place_tooltip(int mx, int my);
	bool focusable(const UiControl *c) const { return c->enabled() && c->visible(); }

	std::list<std::unique_ptr<UiControl>> controls_;
	std::vector<UiControl *> group_;
	std::deque<std::vector<UiControl *>> saved_;
	UiControl *hover_ = nullptr;
	UiControl *focus_ = nullptr;
	int mouse_x_ = -1, mouse_y_ = -1;
	bool capture_ = false;
	bool active_ = true;
	int event_id_ = -1;
	int64_t hover_time_ = 0;
	bool tip_shown_ = false;
	uint32_t parent_handle_ = 0;
	uint32_t tip_page_handle_ = 0;
	std::unique_ptr<Sprite> tip_sprite_;
	std::map<const Image *, std::unique_ptr<ImageObj>> image_objs_;
	bool destroying_ = false;
};

}  // namespace ore
