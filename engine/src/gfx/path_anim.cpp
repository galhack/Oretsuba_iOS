#include "gfx/path_anim.h"

#include <cmath>

namespace ore {

PathAnimator::PathAnimator(Layer *target, int ms, std::vector<Key> keys, int delay, bool temp_visible, int what,
                           bool spline, double ease, bool loop)
    : Task(12003),
      target_(target ? target->handle() : 0),
      seg_ms_(ms > 0 ? ms : 1),
      delay_(delay),
      temp_visible_(temp_visible),
      spline_(spline),
      loop_(loop),
      what_(what),
      ease_(ease) {
	// key 0 = the current state
	Key k0{0, 0, 255};
	if (target) {
		if (what == 2) {
			if (auto *s = dynamic_cast<Sprite *>(target)) k0.x = s->scroll_x, k0.y = s->scroll_y;
		} else {
			k0.x = target->x;
			k0.y = target->y;
		}
		k0.alpha = target->opacity;
	}
	keys_.push_back(k0);
	keys_.insert(keys_.end(), keys.begin(), keys.end());
	if (spline_) solve_spline();
}

Layer *PathAnimator::layer() {
	return Object::as<Layer>(target_);
}

void PathAnimator::solve_spline() {
	// natural cubic spline second-derivative terms per axis (Thomas algorithm)
	size_t n = keys_.size() - 1;
	auto solve = [&](bool ax) {
		std::vector<double> m(n + 1, 0.0);
		if (n < 2) return m;
		std::vector<double> d(n + 1, 0.0), c(n + 1, 0.0), rhs(n + 1, 0.0);
		auto X = [&](size_t k) { return ax ? keys_[k].x : keys_[k].y; };
		for (size_t k = 1; k < n; ++k) rhs[k] = (X(k + 1) - X(k)) - (X(k) - X(k - 1));
		// tridiagonal: m[k-1] + 4 m[k] + m[k+1] = rhs[k]
		c[1] = 1.0 / 4.0;
		d[1] = rhs[1] / 4.0;
		for (size_t k = 2; k < n; ++k) {
			double den = 4.0 - c[k - 1];
			c[k] = 1.0 / den;
			d[k] = (rhs[k] - d[k - 1]) / den;
		}
		m[n - 1] = d[n - 1];
		for (size_t k = n - 1; k-- > 1;) m[k] = d[k] - c[k] * m[k + 1];
		return m;
	};
	mx_ = solve(true);
	my_ = solve(false);
}

void PathAnimator::apply(double x, double y, int alpha) {
	Layer *l = layer();
	if (!l) return;
	if (what_ == 1) l->set_pos(int(x), int(y));
	if (what_ == 2)
		if (auto *s = dynamic_cast<Sprite *>(l)) {
			int dx = int(x) - s->scroll_x, dy = int(y) - s->scroll_y;
			s->src.l += dx, s->src.r += dx, s->src.t += dy, s->src.b += dy;
			s->scroll_x = int(x), s->scroll_y = int(y);
			s->touch();
		}
	l->set_opacity(alpha);
}

void PathAnimator::on_resume(int64_t, int64_t paused_for) {
	if (start_ >= 0) start_ += paused_for;
}

bool PathAnimator::update(int64_t now) {
	Layer *l = layer();
	if (!l) return true;
	if (start_ < 0) start_ = now;
	int64_t t = now - start_;
	if (t < delay_) return false;
	t -= delay_;
	if (temp_visible_ && !shown_) {
		l->set_visible(true);
		shown_ = true;
	}
	const size_t n = keys_.size() - 1;
	if (n == 0) return true;
	const int64_t total = int64_t(n) * seg_ms_;
	if (t >= total) {
		if (loop_) {
			start_ = now - delay_;
			t = 0;
		} else {
			finish();
			return true;
		}
	}
	double T = double(total), tt = double(t);
	if (ease_ > 0) tt = T * std::pow(tt / T, ease_);
	else if (ease_ < 0) tt = T * (1.0 - std::pow(1.0 - tt / T, -ease_));
	int64_t ti = int64_t(tt);
	size_t i = size_t(ti / seg_ms_);
	if (i >= n) i = n - 1;
	double f = double(ti % seg_ms_) / seg_ms_;
	if (ti >= total) f = 1.0;
	const Key &a = keys_[i], &b = keys_[i + 1];
	double x, y;
	if (spline_ && n >= 2) {
		auto sp = [&](double Xi, double Xj, double Mi, double Mj) {
			return Xi + f * ((Xj - Xi) - (2 * Mi + Mj) + f * (3 * Mi + f * (Mj - Mi)));
		};
		x = std::trunc(sp(a.x, b.x, mx_[i], mx_[i + 1]));
		y = std::trunc(sp(a.y, b.y, my_[i], my_[i + 1]));
	} else {
		x = std::trunc(a.x + (b.x - a.x) * f);
		y = std::trunc(a.y + (b.y - a.y) * f);
	}
	int alpha = a.alpha + int((b.alpha - a.alpha) * f);
	apply(x, y, alpha);
	return false;
}

void PathAnimator::finish() {
	const Key &k = keys_.back();
	apply(k.x, k.y, k.alpha);
	if (temp_visible_)
		if (Layer *l = layer()) l->set_visible(false);
}

}  // namespace ore
