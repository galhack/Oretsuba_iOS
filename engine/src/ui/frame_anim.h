// Frame-animation sources (N_20d, list DAT_005034d0; object vtable 0x4edbdc,
// init FUN_004761a0) and the task that plays one (class FUN_004574e0, task
// types 0x32c9/0x32ca; used by the message-window click cursor, N_2fd).
//
// A source holds an ELG with `frames` horizontal frames; playing it draws the
// current frame INTO a target image (the attach sprite's image, e.g. the
// message window, or a page) at (x, y) and invalidates the attached sprites.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/object.h"
#include "core/scheduler.h"
#include "gfx/image.h"

namespace ore {

class Engine;

class FrameAnim : public Object {
public:
	explicit FrameAnim(Engine &e) : engine(e) {}

	Engine &engine;
	ImageRef image;      // +0x08: frames side by side
	int frames = 1;      // +0x88
	int interval = 100;  // +0x8c: ms per frame
	int mode = 0;        // +0x90: 0 loop, 1 ping-pong, other: random frame each interval
	uint8_t keyed = 0;   // +0x91: 1 restore background + alpha blend, 2 colour key
	ImageRef target;     // +0x94: image the frames are drawn into
	int x = 0, y = 0;    // +0x98/+0x9c
	int end_mode = 0;    // +0xa0 (N_20d p8): on finish 0 = show frame 0, 1 = leave, 2 = restore background
	int frame_w = 0;     // +0xa4
	ImageRef saved;      // +0xa8: background under the frame
	std::vector<uint32_t> sprites;  // +0xac: layer handles touched after drawing (N_20e/N_20f)

	// FUN_004761a0. attach = a layer handle (its image is the target) or 0;
	// page = target image when attach is 0. Returns 0 ok, 1 bad args, 2 no
	// target, 3 load failure.
	int init(const std::string &path, int frames, int interval, uint32_t attach, ImageRef page, int x, int y,
	         int mode, int end_mode, bool keyed, bool keep8, uint32_t ck);
	void add_sprite(uint32_t h);     // FUN_004763e0
	void remove_sprite(uint32_t h);  // FUN_00476610

	void save_background();  // FUN_00457940
	void draw_frame(int f);  // FUN_00457c50
	void finish();           // FUN_004579d0
	// Length of one play cycle (task +0x40); 0 for random mode.
	int cycle_ms() const;
	// Frame shown `t` ms into a cycle (FUN_00457b80).
	int frame_at(int64_t t) const;

private:
	void touch_sprites();
};

// Plays a FrameAnim (FUN_004576c0 / FUN_00457550): loops `loops` cycles
// (0 = forever) and calls finish() at the end. Ends by itself when the
// source is freed (N_210 / N_211).
class FrameAnimTask : public Task {
public:
	static constexpr int kType = 0x32c9;
	FrameAnimTask(FrameAnim *anim, int loops, int type = kType);
	bool update(int64_t now) override;
	void finish() override;
	uint32_t anim_handle() const { return anim_; }

private:
	uint32_t anim_;
	int loops_;
	int64_t start_ = -1;
	int last_ = -1;
	uint32_t rng_ = 0x12345;
};

}  // namespace ore
