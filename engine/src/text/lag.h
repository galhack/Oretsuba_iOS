// Frame-animation data loaded by N_1f8 ("LAG", logo.lag) and its player task
// (N_1fc), the "LAD" animation resource of N_1fe (consumed by the bg_anime
// effect) and the lip-sync task (N_214). docs/re/40_text.md §8.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/scheduler.h"
#include "gfx/image.h"
#include "gfx/layer.h"

namespace ore {

class Engine;

class LagData : public Object {
public:
	struct Entry {
		int image = -1;  // < 0: end of a sequence
		uint32_t ms = 0;
		int x = 0, y = 0;
	};
	bool load(Engine &e, const std::string &path, bool alpha_images);  // FUN_0046ed00
	int width = 0, height = 0;
	std::vector<Entry> entries;   // with a final -1 sentinel
	std::vector<int> seq_start;   // first entry of each sequence
	std::vector<ImageRef> images;
	std::string path;
};

// FUN_00457390 / FUN_00457250: plays one sequence of a LagData as a sprite.
class LagPlayer : public Task {
public:
	static constexpr int kType = 13000;
	LagPlayer() : Task(kType) {}
	int start(LagData *d, LayerList *list, int seq, int loops, bool catch_up, bool remove_at_end);
	bool update(int64_t now) override;
	void finish() override;

	uint32_t data = 0;  // LagData handle
	Sprite sprite{nullptr, nullptr, 0, 0};
	size_t first = 0, cur = 0;
	int loops = 0;
	bool catch_up = false, remove_at_end = false;
	int state = 0;  // +0xf4: 0 armed, 1 playing, 2 stopped
	int64_t t0 = 0;

private:
	void show(size_t idx);
};

// "LAD" resource (N_1fe): kept raw for the bg_anime effect.
class AnimData : public Object {
public:
	std::string path;
	std::vector<uint8_t> data;
	uint16_t field_8 = 0, field_a = 0, field_e = 0;
	uint32_t field_18 = 0;
};

// FUN_00457e50 class: mouth animation (an N_20d FrameAnim) frame from the voice
// level of a sound channel.
class LipSyncTask : public Task {
public:
	static constexpr int kType = 13002;
	LipSyncTask(Engine &e, uint32_t target, int ch) : Task(kType), e_(e), target_(target), ch_(ch) {}
	bool update(int64_t now) override;
	void finish() override;

private:
	Engine &e_;
	uint32_t target_;
	int ch_;
	int frame_ = -1;
};

}  // namespace ore
