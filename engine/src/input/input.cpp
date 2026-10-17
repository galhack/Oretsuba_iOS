#include "input/input.h"

#include <cstring>

namespace ore {

int Input::vk_to_dik(int vk) {
	if (vk >= 'A' && vk <= 'Z') {
		static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
		                                    0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
		                                    0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
		return letters[vk - 'A'];
	}
	if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
	if (vk == '0') return 0x0B;
	if (vk >= 0x70 && vk <= 0x79) return 0x3B + (vk - 0x70);  // F1..F10
	switch (vk) {
	case 0x0D: return 0x1C;  // Return
	case 0x1B: return 0x01;  // Escape
	case 0x20: return 0x39;  // Space
	case 0x09: return 0x0F;  // Tab
	case 0x08: return 0x0E;  // Backspace
	case 0x10: return 0x2A;  // Shift
	case 0x11: return 0x1D;  // Control
	case 0x12: return 0x38;  // Alt
	case 0x25: return 0xCB;  // Left
	case 0x26: return 0xC8;  // Up
	case 0x27: return 0xCD;  // Right
	case 0x28: return 0xD0;  // Down
	case 0x21: return 0xC9;  // PageUp
	case 0x22: return 0xD1;  // PageDown
	case 0x24: return 0xC7;  // Home
	case 0x23: return 0xCF;  // End
	case 0x7A: return 0x57;  // F11
	case 0x7B: return 0x58;  // F12
	}
	return -1;
}

void Input::on_key(int vk, bool down) {
	int dik = vk_to_dik(vk);
	if (dik < 0) return;
	if (down) latched_keys_[dik] = true;
	host_keys_[dik] = down;
}

void Input::on_mouse_move(int x, int y) {
	if (x != x_ || y != y_) host_moved_ = true;
	x_ = x;
	y_ = y;
	inside_ = x >= 0 && y >= 0 && x < 800 && y < 600;
}

void Input::on_mouse_button(int button, bool down) {
	if (button < 0 || button > 2) return;
	if (down) latched_buttons_[button] = true;
	host_buttons_[button] = down;
}

void Input::on_wheel(int delta) {
	host_wheel_ += delta;
}

void Input::on_command(int command) {
	if (command >= 0 && command < 12) commands_pending_ |= uint16_t(1u << command);
}

void Input::poll() {
	commands_cur_ = commands_pending_;
	commands_pending_ = 0;
	memcpy(prev_, cur_, sizeof cur_);
	for (int i = 0; i < 256; ++i) {
		cur_[i] = host_keys_[i] || latched_keys_[i];
		latched_keys_[i] = false;
	}
	for (int i = 0; i < 3; ++i) {
		btn_prev_[i] = btn_cur_[i];
		btn_cur_[i] = (host_buttons_[i] || latched_buttons_[i]) && inside_;
		latched_buttons_[i] = false;
	}
	wheel_prev_ = wheel_cur_;
	wheel_cur_ = host_wheel_;
	host_wheel_ = 0;
	moved_ = host_moved_;
	host_moved_ = false;
}

void Input::clear() {
	commands_pending_ = commands_cur_ = 0;
	memset(cur_, 0, sizeof cur_);
	memset(prev_, 0, sizeof prev_);
	memset(btn_cur_, 0, sizeof btn_cur_);
	memset(btn_prev_, 0, sizeof btn_prev_);
	wheel_cur_ = wheel_prev_ = 0;
}

void Input::clear_edges() {
	commands_cur_ = 0;
	memcpy(prev_, cur_, sizeof cur_);
	memcpy(btn_prev_, btn_cur_, sizeof btn_cur_);
	wheel_prev_ = wheel_cur_;
}

int Input::first_key_held() const {
	for (int i = 1; i < 256; ++i)
		if (key_held(i)) return i;
	return -1;
}

int Input::first_key_pressed() const {
	for (int i = 1; i < 256; ++i)
		if (key_pressed(i)) return i;
	return -1;
}

bool Input::mouse_held(int code) const {
	if (code >= 0 && code < 3) return btn_cur_[code];
	if (code == 3) return wheel_cur_ > 0;
	if (code == 4) return wheel_cur_ < 0;
	return false;
}

bool Input::mouse_pressed(int code) const {
	if (code >= 0 && code < 3) return btn_cur_[code] && !btn_prev_[code];
	if (code == 3) return wheel_prev_ == 0 && wheel_cur_ > 0;
	if (code == 4) return wheel_prev_ == 0 && wheel_cur_ < 0;
	return false;
}

int Input::first_mouse_held() const {
	for (int c = 0; c < 5; ++c)
		if (mouse_held(c)) return c;
	return -1;
}

int Input::first_mouse_pressed() const {
	for (int c = 0; c < 5; ++c)
		if (mouse_pressed(c)) return c;
	return -1;
}

int Input::edge_code() const {
	if (!inside_) return 0;
	bool top = y_ <= 0, bottom = y_ >= 599, left = x_ <= 0, right = x_ >= 799;
	if (top && left) return 7;
	if (top && right) return 9;
	if (bottom && left) return 1;
	if (bottom && right) return 3;
	if (top) return 8;
	if (bottom) return 2;
	if (left) return 4;
	if (right) return 6;
	return 0;
}

}  // namespace ore
