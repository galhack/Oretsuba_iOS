#include "core/scheduler.h"

#include <algorithm>

namespace ore {

int Scheduler::add(std::unique_ptr<Task> t, int64_t interval, int count) {
	if (!t) return 1;
	if (contains(t.get())) return 2;
	t->interval = interval;
	t->count = count;
	t->next = now_;
	t->killed = false;
	if (ticking_)
		pending_.push_back(std::move(t));
	else
		tasks_.push_back(std::move(t));
	return 0;
}

bool Scheduler::kill(Task *t) {
	for (auto &p : tasks_)
		if (p.get() == t && !p->killed) {
			p->killed = true;
			p->finish();
			return true;
		}
	for (auto &p : pending_)
		if (p.get() == t && !p->killed) {
			p->killed = true;
			p->finish();
			return true;
		}
	return false;
}

void Scheduler::kill_type(int type_id) {
	for (auto &p : tasks_)
		if (!p->killed && p->type_id() == type_id) {
			p->killed = true;
			p->finish();
		}
	for (auto &p : pending_)
		if (!p->killed && p->type_id() == type_id) {
			p->killed = true;
			p->finish();
		}
}

void Scheduler::clear() {
	tasks_.clear();
	pending_.clear();
}

bool Scheduler::contains(const Task *t) const {
	for (auto &p : tasks_)
		if (p.get() == t) return true;
	for (auto &p : pending_)
		if (p.get() == t) return true;
	return false;
}

void Scheduler::pause(Task *t) {
	if (!t) return;
	if (t->paused++ == 0) {
		t->paused_at = now_;
		t->on_pause(now_);
	}
}

void Scheduler::pause_type(int type_id) {
	for (auto &p : tasks_)
		if (type_id < 0 || p->type_id() == type_id) pause(p.get());
}

void Scheduler::resume(Task *t) {
	if (!t || t->paused == 0) return;
	if (--t->paused == 0) {
		int64_t d = now_ - t->paused_at;
		t->next += d;
		t->on_resume(now_, d);
	}
}

void Scheduler::resume_type(int type_id) {
	for (auto &p : tasks_)
		if (type_id < 0 || p->type_id() == type_id) resume(p.get());
}

void Scheduler::tick(int64_t now) {
	now_ = now;
	ticking_ = true;
	for (size_t i = 0; i < tasks_.size(); ++i) {
		Task *t = tasks_[i].get();
		if (t->killed || t->paused || now < t->next) continue;
		bool done = t->update(now);
		if (done) {
			t->killed = true;
			continue;
		}
		if (t->count > 0 && --t->count == 0) {
			t->killed = true;
			continue;
		}
		t->next += t->interval;
		if (t->next <= now) t->next = now + t->interval;
	}
	ticking_ = false;
	tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [](const std::unique_ptr<Task> &t) { return t->killed; }),
	             tasks_.end());
	for (auto &p : pending_) tasks_.push_back(std::move(p));
	pending_.clear();
}

}  // namespace ore
