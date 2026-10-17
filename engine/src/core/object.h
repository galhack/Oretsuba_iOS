// Script-visible engine objects and their handles.
//
// Natives hand scripts opaque 32-bit handles (the original returned C++
// object pointers). Every Object registers itself on construction and
// unregisters on destruction, so a stale handle simply resolves to null.
#pragma once

#include <cstdint>
#include <unordered_map>

namespace ore {

class Object {
public:
	Object();
	virtual ~Object();
	Object(const Object &) = delete;
	Object &operator=(const Object &) = delete;

	uint32_t handle() const { return handle_; }

	// Graphics type as the original's byte `vt+4`: 4 image, 7/9/10 surface
	// pages, 0 other; -1 = not a graphics object.
	virtual int gfx_type() const { return -1; }
	// UI control type (docs/re/60_ui.md §3): 0..9, 0x6f; -1 = not a control.
	virtual int ui_type() const { return -1; }

	static Object *from_handle(uint32_t h);
	template <class T>
	static T *as(uint32_t h) {
		return dynamic_cast<T *>(from_handle(h));
	}

private:
	uint32_t handle_;
};

// Handles live in their own range so they never look like VM addresses.
constexpr uint32_t kHandleBase = 0x60000000;

}  // namespace ore
