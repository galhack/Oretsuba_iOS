// Keyboard/mouse state as the original exposes it to scripts
// (docs/re/50_audio_movie_input.md §3): DirectInput scan codes with held and
// pressed-this-frame state, mouse codes 0 left, 1 right, 2 middle, 3 wheel up,
// 4 wheel down, cursor in game coordinates (-1 when outside).
#pragma once

#include <cstdint>

namespace ore {

class Input {
public:
	// Host events (any time between frames).
	void on_key(int vk, bool down);
	void on_mouse_move(int x, int y);
	void on_mouse_button(int button, bool down);
	void on_wheel(int delta);
	// One-shot script command (0..11), independent of key bindings.
	void on_command(int command);
	bool command_pressed(int command) const {
		return command >= 0 && command < 12 && (commands_cur_ & (1u << command));
	}
	void on_leave() { inside_ = false; }

	// Samples the host state into the frame state; call once per frame.
	void poll();
	void clear();
	// Makes this frame's edges invisible to further VM runs in the same frame.
	void clear_edges();

	bool key_held(int dik) const { return dik >= 0 && dik < 256 && cur_[dik]; }
	bool key_pressed(int dik) const { return dik >= 0 && dik < 256 && cur_[dik] && !prev_[dik]; }
	int first_key_held() const;
	int first_key_pressed() const;

	bool mouse_held(int code) const;
	bool mouse_pressed(int code) const;
	int wheel() const { return wheel_cur_; }
	int first_mouse_held() const;
	int first_mouse_pressed() const;

	int cursor_x() const { return inside_ ? x_ : -1; }
	int cursor_y() const { return inside_ ? y_ : -1; }
	// 0 inside; 8 top, 2 bottom, 4 left, 6 right, 7/9/1/3 corners.
	int edge_code() const;
	bool mouse_moved() const { return moved_; }

	void warp(int x, int y) {
		x_ = x;
		y_ = y;
	}

	static int vk_to_dik(int vk);

private:
	// host side
	bool host_keys_[256] = {};
	bool latched_keys_[256] = {};  // pressed and released before a poll
	bool host_buttons_[3] = {};
	bool latched_buttons_[3] = {};
	int host_wheel_ = 0;
	int x_ = 400, y_ = 300;
	bool inside_ = true;
	bool host_moved_ = false;
	uint16_t commands_pending_ = 0, commands_cur_ = 0;
	// frame side
	bool cur_[256] = {}, prev_[256] = {};
	bool btn_cur_[3] = {}, btn_prev_[3] = {};
	int wheel_cur_ = 0, wheel_prev_ = 0;
	bool moved_ = false;
};

}  // namespace ore
