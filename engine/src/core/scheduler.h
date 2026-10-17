// Task scheduler (docs/re/20_graphics_core.md §1.2): typed tasks ticked with
// a millisecond clock; kill = jump to the end state; pause shifts time.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/object.h"

namespace ore {

class Task : public Object {
public:
	explicit Task(int type_id) : type_id_(type_id) {}
	int type_id() const { return type_id_; }

	// Returns true when finished (the task is then deleted).
	virtual bool update(int64_t now) = 0;
	// Jump to the end state (called when killed).
	virtual void finish() {}
	// Called when the scheduler pauses/resumes this task; default shifts start.
	virtual void on_pause(int64_t /*now*/) {}
	virtual void on_resume(int64_t /*now*/, int64_t /*paused_for*/) {}

	int64_t next = 0;
	int64_t interval = 0;
	int count = 0;  // remaining repeats, 0 = infinite
	bool killed = false;
	int paused = 0;
	int64_t paused_at = 0;

private:
	int type_id_;
};

class Scheduler {
public:
	~Scheduler() { clear(); }

	int add(std::unique_ptr<Task> t, int64_t interval = 0, int count = 0);
	bool kill(Task *t);
	void kill_type(int type_id);
	void clear();
	bool contains(const Task *t) const;
	void pause(Task *t);
	void pause_type(int type_id);  // < 0 = all
	void resume(Task *t);
	void resume_type(int type_id);
	void tick(int64_t now);
	int64_t now() const { return now_; }
	void set_now(int64_t now) { now_ = now; }
	size_t size() const { return tasks_.size(); }

private:
	std::vector<std::unique_ptr<Task>> tasks_;
	int64_t now_ = 0;
	bool ticking_ = false;
	std::vector<std::unique_ptr<Task>> pending_;  // added during a tick
};

}  // namespace ore
