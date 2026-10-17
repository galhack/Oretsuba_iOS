// Path animator task (N_1b8/N_1b9, docs/re/20_graphics_core.md §4).
#pragma once

#include <vector>

#include "core/scheduler.h"
#include "gfx/layer.h"

namespace ore {

class PathAnimator : public Task {
public:
	struct Key {
		double x, y;
		int alpha;
	};
	// what: 0 opacity only, 1 position, 2 sprite source scroll offset.
	PathAnimator(Layer *target, int ms_per_segment, std::vector<Key> keys, int delay, bool temp_visible, int what,
	             bool spline, double ease, bool loop);
	bool update(int64_t now) override;
	void finish() override;
	void on_resume(int64_t now, int64_t paused_for) override;
	uint32_t target_handle() const { return target_; }

private:
	void apply(double x, double y, int alpha);
	Layer *layer();
	void solve_spline();
	uint32_t target_;
	int seg_ms_, delay_;
	bool temp_visible_, spline_, loop_;
	int what_;
	double ease_;
	std::vector<Key> keys_;
	std::vector<double> mx_, my_;
	int64_t start_ = -1;
	bool shown_ = false;
};

}  // namespace ore
