// Layer / background effects N_d6..N_d9 and the zoom pulse N_35 (docs/re/30_effects.md §5).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

#include "core/args.h"
#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/effects/fx_util.h"
#include "gfx/effects/move_lang.h"
#include "gfx/effects/transitions.h"
#include "gfx/layer.h"
#include "util/log.h"

namespace ore {

Layer *layer_of(uint32_t h);  // gfx/natives_gfx.cpp

namespace {

using fx::icos;

enum { kTaskBg = 14000, kTaskSpMove = 14001, kTaskOverlay = 14003, kTaskBgAnime = 15000 };

// Priorities used by the move language's 'f' / 'b' (set by N_d9("sp_move", 0, front, behind)).
int g_front_prio = 2000, g_behind_prio = 2006;

class LayerEffect : public Task {
public:
	using Task::Task;
	// 0 = running (added to the scheduler), -1 = applied at once (no task), > 0 = error.
	virtual int init(Engine &e, Surface *page, Args &a, int k) = 0;
	virtual int control(Args &, int) { return 0; }
	virtual int query(Args &, int) { return 0; }
};

// ---------------------------------------------------------------- background helpers

// The background state an effect works on; follows the content across page swaps.
struct BgRef {
	Engine *e = nullptr;
	uint32_t page_h = 0;  // the page may be deleted while the effect runs
	ImageRef bg;
	ImageRef work;  // our bg_cache image (if any)
	int w = Engine::kWidth, h = Engine::kHeight;
	Surface *page() const { return Object::as<Surface>(page_h); }
	Compositor *comp() const { return e ? fx::find_comp(*e, page(), bg, work) : nullptr; }
	bool attach(Engine &eng, Surface *p) {
		e = &eng;
		if (!p || !p->comp()) return false;
		page_h = p->handle();
		bg = p->comp()->bg;
		w = p->image().width();
		h = p->image().height();
		return true;
	}
	int W() const { return w; }
	int H() const { return h; }
	// Installs a page-sized work image as the compositor's bg_cache.
	Image &install_work(Compositor *c) {
		if (!work) work = std::make_shared<Image>(W(), H(), 32);
		if (c) c->bg_cache = work;
		return *work;
	}
	void remove_work() {
		if (!work) return;
		if (Compositor *c = comp())
			if (c->bg_cache == work) c->bg_cache.reset();
		work.reset();
		gfx_touch();
	}
};

// Last zoom view / rotation per background image (the compositor's cache keys).
struct BgView {
	Rect view;
	bool has_view = false;
	int cx = 0, cy = 0, angle = 0;
	double zoom = 1.0;
};
std::map<const Image *, BgView> g_views;

void apply_view(Compositor *c, int W, int H, const Rect &r) {
	if (!c || !c->bg) return;
	g_views[c->bg.get()].view = r;
	g_views[c->bg.get()].has_view = true;
	if (r.w() == W && r.h() == H) {
		c->bg_cache.reset();
		c->bg_x = r.l, c->bg_y = r.t;
	} else {
		auto cache = std::make_shared<Image>(W, H, 32);
		fill_rect(*cache, cache->rect(), c->bg_color & 0xffffff);
		if (r.w() > 0 && r.h() > 0) stretch_blit(*cache, *c->bg, 0, 0, W, H, r, 0, 0, 255, false);
		c->bg_cache = cache;
	}
	gfx_touch();
}

void apply_rot(Compositor *c, int W, int H, int cx, int cy, int angle, double zoom) {
	if (!c || !c->bg) return;
	BgView &v = g_views[c->bg.get()];
	v.cx = cx, v.cy = cy, v.angle = angle, v.zoom = zoom;
	if (angle == 0 && std::fabs(zoom - 1.0) < 1e-9) {
		c->bg_cache.reset();
		gfx_touch();
		return;
	}
	auto cache = std::make_shared<Image>(W, H, 32);
	fill_rect(*cache, cache->rect(), c->bg_color & 0xffffff);
	double co = fx::dcos(angle), si = fx::dcos(angle - 128);
	// screen = R·zoom·(p − c) + c − bg offset
	double A = co * zoom, B = -si * zoom, C = si * zoom, D = co * zoom;
	double tx = cx - c->bg_x - (A * cx + B * cy), ty = cy - c->bg_y - (C * cx + D * cy);
	affine_blit(*cache, *c->bg, c->bg->rect(), A, B, C, D, tx, ty, 0, 255, false);
	c->bg_cache = cache;
	gfx_touch();
}

// ---------------------------------------------------------------- bg_rast / bg_haze

class BgWave : public LayerEffect {
public:
	explicit BgWave(bool haze) : LayerEffect(kTaskBg), haze_(haze) {}

	int init(Engine &e, Surface *page, Args &a, int k) override {
		if (!ref_.attach(e, page) || !ref_.bg) return 1;
		if (haze_) {
			n_ = std::max(1, a.i(k + 1));
			build_table(a.s(k), n_);
			amp_ = 512;
		} else {
			amp_ = a.i(k);
			lambda_ = std::max(1, a.i(k + 1));
		}
		period_ = std::max(1, a.i(k + 2));
		dur_ = a.i(k + 3);
		fade_in_ = a.i(k + 4);
		fade_out_ = a.i(k + 5);
		fill_ = a.b(k + 6);
		Compositor *c = page->comp();
		Image &w = ref_.install_work(c);
		if (!fill_) copy_window(c, w);
		gfx_touch();
		return 0;
	}

	bool update(int64_t now) override {
		if (start_ < 0) start_ = now;
		int64_t t = now - start_;
		int A = 0;
		if (state_ == 0) {
			if (fade_in_ <= 0 || t >= fade_in_) {
				state_ = 1;
			} else {
				A = int(int64_t(amp_) * t / fade_in_);
			}
		}
		if (state_ == 1) {
			A = amp_;
			if (dur_ != 0 && t - fade_in_ >= dur_) state_ = 2;
		}
		if (state_ == 2) {
			t0_ = t;
			a0_ = amp_;
			state_ = 3;
		}
		if (state_ == 3) {
			if (fade_out_ <= 0 || t - t0_ >= fade_out_) {
				finish();
				return true;
			}
			A = a0_ - int(int64_t(a0_) * (t - t0_) / fade_out_);
		}
		cur_a_ = A;
		Compositor *c = ref_.comp();
		if (!c) return false;
		if (c->bg_cache != ref_.work) ref_.install_work(c);
		int phase = haze_ ? int((t % period_) * n_ / period_) : int(((t % period_) << 9) / period_);
		if (phase == last_phase_ && A == last_a_ && c->bg_x == last_x_ && c->bg_y == last_y_) return false;
		last_phase_ = phase, last_a_ = A, last_x_ = c->bg_x, last_y_ = c->bg_y;
		if (haze_)
			render_haze(c, phase, A);
		else
			render_rast(c, phase, A);
		gfx_touch();
		return false;
	}

	void finish() override { ref_.remove_work(); }

	int control(Args &, int) override {
		// start the fade-out (from the current amplitude when still fading in)
		if (state_ < 2) {
			state_ = 3;
			t0_ = last_t();
			a0_ = cur_a_;
		}
		return 0;
	}
	int query(Args &, int) override { return state_; }

private:
	int64_t last_t() const { return start_ < 0 ? 0 : ref_.e->now - start_; }

	void build_table(const char *s, int n) {
		struct G {
			int f, ph, a;
		};
		std::vector<G> groups;
		for (const char *p = s; p && (p = strchr(p, '('));) {
			++p;
			G g{0, 0, 0};
			g.f = atoi(p);
			const char *q = strchr(p, ',');
			if (q) g.ph = atoi(++q), q = strchr(q, ',');
			if (q) g.a = atoi(++q);
			groups.push_back(g);
		}
		T_.assign(size_t(n), 0);
		if (groups.empty()) return;
		for (int i = 0; i < n; ++i) {
			int64_t sum = 0;
			for (const G &g : groups) sum += int64_t(icos(((i * 512 / n) * g.f + g.ph) & 511)) * g.a;
			T_[size_t(i)] = int((sum / int64_t(groups.size())) >> 16);
		}
	}

	void copy_window(Compositor *c, Image &w) {
		if (!c || !c->bg) return;
		copy_rect(w, 0, 0, *c->bg, Rect(c->bg_x, c->bg_y, c->bg_x + w.width(), c->bg_y + w.height()));
	}

	// Base pixel at window coordinates (x, y); false if outside the background.
	static bool base_px(const Compositor *c, int x, int y, uint32_t &out) {
		const Image &b = *c->bg;
		int bx = x + c->bg_x, by = y + c->bg_y;
		if (bx < 0 || by < 0 || bx >= b.width() || by >= b.height()) return false;
		out = b.pixel(bx, by);
		return true;
	}

	void render_rast(Compositor *c, int phase, int A) {
		Image &w = *ref_.work;
		uint32_t ph = uint32_t(phase) << 16, step = 0x2000000u / uint32_t(lambda_);
		uint32_t fillc = c->bg_color & 0xffffff;
		for (int y = 0; y < w.height(); ++y, ph += step) {
			int dx = int((int64_t(icos(int((ph >> 16) - 128) & 511)) * A) >> 16);
			uint32_t *d = w.row32(y);
			for (int x = 0; x < w.width(); ++x) {
				uint32_t p;
				if (base_px(c, x - dx, y, p))
					d[x] = p;
				else if (fill_)
					d[x] = fillc;
			}
		}
	}

	void render_haze(Compositor *c, int off, int A) {
		Image &w = *ref_.work;
		uint32_t fillc = c->bg_color & 0xffffff;
		for (int y = 0; y < w.height(); ++y) {
			int k = (off + y) % n_;
			int d = (T_[size_t(k)] * A) >> 9;
			uint32_t *row = w.row32(y);
			for (int x = 0; x < w.width(); ++x) {
				uint32_t p;
				if (base_px(c, x + d, y + d, p))
					row[x] = p;
				else if (fill_)
					row[x] = fillc;
			}
		}
	}

	bool haze_;
	BgRef ref_;
	std::vector<int> T_;
	int n_ = 1, amp_ = 0, lambda_ = 1, period_ = 1, dur_ = 0, fade_in_ = 0, fade_out_ = 0;
	bool fill_ = false;
	int state_ = 0, a0_ = 0, cur_a_ = 0;
	int64_t start_ = -1, t0_ = 0;
	int last_phase_ = -1, last_a_ = -1, last_x_ = 0, last_y_ = 0;
};

// ---------------------------------------------------------------- bg_zoom / bg_rot

Rect zoom_target(const Compositor *c, int W, int H, int cx, int cy, double z) {
	if (z <= 0) z = 1.0;
	int w = int(std::trunc(W / z)), h = int(std::trunc(H / z));
	int iw = c->bg->width(), ih = c->bg->height();
	int x, y;
	if (w > iw)
		x = 0, w = iw;
	else
		x = std::clamp(cx - w / 2, 0, iw - w);
	if (h > ih)
		y = 0, h = ih;
	else
		y = std::clamp(cy - h / 2, 0, ih - h);
	return Rect(x, y, x + w, y + h);
}

class BgZoom : public LayerEffect {
public:
	BgZoom() : LayerEffect(kTaskBg) {}
	int init(Engine &e, Surface *page, Args &a, int k) override {
		if (!ref_.attach(e, page) || !ref_.bg) return 1;
		Compositor *c = page->comp();
		W_ = ref_.W(), H_ = ref_.H();
		target_ = zoom_target(c, W_, H_, a.i(k), a.i(k + 1), fx::atof_or(a.s(k + 2), 1.0));
		ease_ = fx::atof_or(a.s(k + 3), 0.0);
		d_ = a.i(k + 4);
		auto it = g_views.find(c->bg.get());
		from_ = (it != g_views.end() && it->second.has_view && c->bg_cache) ? it->second.view
		                                                                  : Rect(c->bg_x, c->bg_y, c->bg_x + W_, c->bg_y + H_);
		if (d_ <= 0) {
			apply_view(c, W_, H_, target_);
			return -1;
		}
		return 0;
	}
	bool update(int64_t now) override {
		if (start_ < 0) start_ = now;
		int64_t t = now - start_;
		Compositor *c = ref_.comp();
		if (t >= d_) {
			finish();
			return true;
		}
		double p = double(fx::ease_ms(t, d_, ease_) % d_) / d_;
		if (p >= 0.999 && p <= 1.001) p = 1.0;
		auto lerp = [p](int a, int b) { return int(std::trunc(a + (b - a) * p)); };
		Rect r(lerp(from_.l, target_.l), lerp(from_.t, target_.t), lerp(from_.r, target_.r), lerp(from_.b, target_.b));
		apply_view(c, W_, H_, r);
		return false;
	}
	void finish() override {
		if (done_) return;
		done_ = true;
		apply_view(ref_.comp(), W_, H_, target_);
	}
	int query(Args &a, int k) override {
		a.out(k, target_.l), a.out(k + 1, target_.t), a.out(k + 2, target_.w()), a.out(k + 3, target_.h());
		return 0;
	}

private:
	BgRef ref_;
	Rect from_, target_;
	double ease_ = 0;
	int d_ = 0, W_ = 0, H_ = 0;
	int64_t start_ = -1;
	bool done_ = false;
};

class BgRot : public LayerEffect {
public:
	BgRot() : LayerEffect(kTaskBg) {}
	int init(Engine &e, Surface *page, Args &a, int k) override {
		if (!ref_.attach(e, page) || !ref_.bg) return 1;
		Compositor *c = page->comp();
		auto it = g_views.find(c->bg.get());
		if (it != g_views.end()) s_cx_ = it->second.cx, s_cy_ = it->second.cy, s_a_ = it->second.angle, s_z_ = it->second.zoom;
		cx_ = a.i(k), cy_ = a.i(k + 1), a_ = a.i(k + 2);
		z_ = fx::atof_or(a.s(k + 3), 1.0);
		if (z_ == 0) z_ = 1.0;
		ease_ = fx::atof_or(a.s(k + 4), 0.0);
		d_ = a.i(k + 5);
		if (d_ <= 0) {
			finish();
			return -1;
		}
		return 0;
	}
	bool update(int64_t now) override {
		if (start_ < 0) start_ = now;
		int64_t t = now - start_;
		if (t >= d_) {
			finish();
			return true;
		}
		double p = double(fx::ease_ms(t, d_, ease_)) / d_;
		auto li = [p](int a, int b) { return int(std::trunc(a + (b - a) * p)); };
		apply_rot(ref_.comp(), ref_.W(), ref_.H(), li(s_cx_, cx_), li(s_cy_, cy_), li(s_a_, a_), s_z_ + (z_ - s_z_) * p);
		return false;
	}
	void finish() override {
		if (done_) return;
		done_ = true;
		apply_rot(ref_.comp(), ref_.W(), ref_.H(), cx_, cy_, a_, z_);
	}

private:
	BgRef ref_;
	int s_cx_ = 0, s_cy_ = 0, s_a_ = 0;
	double s_z_ = 1.0;
	int cx_ = 0, cy_ = 0, a_ = 0, d_ = 0;
	double z_ = 1.0, ease_ = 0;
	int64_t start_ = -1;
	bool done_ = false;
};

// ---------------------------------------------------------------- bg_blur

class BgBlur : public LayerEffect {
public:
	BgBlur() : LayerEffect(kTaskBg) {}
	int init(Engine &e, Surface *page, Args &a, int k) override {
		if (!ref_.attach(e, page)) return 1;
		Compositor *c = page->comp();
		int j;
		if (a.u(k) != 0) {
			// key-frame form "(x,y,z)(x,y,z)…"
			for (const char *p = a.s(k); (p = strchr(p, '('));) {
				++p;
				Key key{0, 0, 1.0};
				key.x = atof(p);
				const char *q = strchr(p, ',');
				if (q) key.y = atof(++q), q = strchr(q, ',');
				if (q) key.z = atof(++q);
				keys_.push_back(key);
			}
			if (keys_.empty()) return 3;
			j = k + 2;
		} else {
			Key k0{double(a.i(k + 1)), double(a.i(k + 2)), fx::atof_or(a.s(k + 3), 1.0)};
			Key k1{k0.x, k0.y, fx::atof_or(a.s(k + 5), 1.0)};
			a0_ = int(std::trunc(fx::atof_or(a.s(k + 4), 0.0) * -512.0));
			a1_ = int(std::trunc(fx::atof_or(a.s(k + 6), 0.0) * -512.0));
			keys_ = {k0, k1};
			j = k + 7;
		}
		d_ = a.i(j);
		opa_ = int(a.u(j + 1) & 0xff);
		ease_ = fx::atof_or(a.s(j + 2), 0.0);
		if (a.u(j + 3)) {
			overlay_ = e.load_image(a.s(j + 3), false);
			if (!overlay_) return 4;
		} else {
			if (!c->bg) return 5;
			overlay_ = std::make_shared<Image>(ref_.W(), ref_.H(), 32);
			copy_rect(*overlay_, 0, 0, *c->bg, Rect(c->bg_x, c->bg_y, c->bg_x + ref_.W(), c->bg_y + ref_.H()));
		}
		if (a.b(j + 4)) flip(true);
		if (a.b(j + 5)) flip(false);
		keep_ = a.b(j + 6);
		filter_ = a.b(j + 7) ? 2 : 0;
		auto_remove_ = a.b(j + 9);
		if (keep_) {
			if (!c->bg) return 7;
		} else {
			Image &w = ref_.install_work(c);
			if (c->bg)
				copy_rect(w, 0, 0, *c->bg, Rect(c->bg_x, c->bg_y, c->bg_x + w.width(), c->bg_y + w.height()));
			else
				fill_rect(w, w.rect(), c->bg_color & 0xffffff);
		}
		draw(0.0);
		return 0;
	}
	bool update(int64_t now) override {
		if (start_ < 0) start_ = now;
		int64_t t = now - start_;
		if (held_) return false;
		if (t >= d_) {
			draw(1.0);
			if (auto_remove_) {
				ref_.remove_work();
				return true;
			}
			held_ = true;
			return false;
		}
		if (now - last_ < 16) return false;
		last_ = now;
		draw(fx::ease_p(double(t) / std::max(1, d_), ease_));
		return false;
	}
	void finish() override { ref_.remove_work(); }

private:
	struct Key {
		double x, y, z;
	};
	void flip(bool horiz) {
		Image &o = *overlay_;
		for (int y = 0; y < o.height(); ++y)
			if (horiz) std::reverse(o.row32(y), o.row32(y) + o.width());
		if (!horiz)
			for (int y = 0; y < o.height() / 2; ++y)
				std::swap_ranges(o.row32(y), o.row32(y) + o.width(), o.row32(o.height() - 1 - y));
	}
	void draw(double p) {
		Compositor *c = ref_.comp();
		if (!c) return;
		Image *dst = keep_ ? c->bg.get() : ref_.work.get();
		if (!dst) return;
		if (!keep_ && c->bg_cache != ref_.work) ref_.install_work(c);
		// position along the key list (linear)
		double fpos = p * double(keys_.size() - 1);
		size_t i = std::min(keys_.size() - 1, size_t(fpos));
		size_t i1 = std::min(keys_.size() - 1, i + 1);
		double f = fpos - double(i);
		double cx = keys_[i].x + (keys_[i1].x - keys_[i].x) * f, cy = keys_[i].y + (keys_[i1].y - keys_[i].y) * f;
		double z = keys_[i].z + (keys_[i1].z - keys_[i].z) * f;
		int ang = int(std::trunc(a0_ + (a1_ - a0_) * p));
		double ox = keep_ ? c->bg_x : 0, oy = keep_ ? c->bg_y : 0;
		const Image &o = *overlay_;
		if (ang == 0) {
			int w = int(o.width() * z), h = int(o.height() * z);
			int x = int(cx - cx * z + ox), y = int(cy - cy * z + oy);
			stretch_blit(*dst, o, x, y, w, h, o.rect(), 0, filter_, opa_, false);
		} else {
			double co = fx::dcos(ang), si = fx::dcos(ang - 128);
			double A = co * z, B = -si * z, C = si * z, D = co * z;
			double tx = cx - (A * cx + B * cy) + ox, ty = cy - (C * cx + D * cy) + oy;
			affine_blit(*dst, o, o.rect(), A, B, C, D, tx, ty, 0, opa_, false);
		}
		gfx_touch();
	}

	BgRef ref_;
	ImageRef overlay_;
	std::vector<Key> keys_;
	int a0_ = 0, a1_ = 0, d_ = 0, opa_ = 255, filter_ = 0;
	double ease_ = 0;
	bool keep_ = false, auto_remove_ = true, held_ = false;
	int64_t start_ = -1, last_ = -1000;
};

// ---------------------------------------------------------------- sp_move / bg_move

class MoveTask : public LayerEffect {
public:
	explicit MoveTask(bool sprite) : LayerEffect(sprite ? kTaskSpMove : kTaskBg), sprite_(sprite) {}

	static bool context(Engine &e, Sprite *s, Compositor *c, Surface *page, fx::MoveContext &mc) {
		mc.W = page ? page->image().width() : Engine::kWidth;
		mc.H = page ? page->image().height() : Engine::kHeight;
		if (s) {
			mc.sprite = true;
			mc.iw = s->src.w(), mc.ih = s->src.h();
			mc.home_x = s->image ? s->image->offset_x : 0;
			mc.home_y = s->image ? s->image->offset_y : 0;
			mc.cur_x = s->x, mc.cur_y = s->y;
			return true;
		}
		if (!c || !c->bg || c->bg_cache) return false;
		mc.iw = c->bg->width(), mc.ih = c->bg->height();
		mc.cur_x = c->bg_x, mc.cur_y = c->bg_y;
		(void)e;
		return true;
	}

	int init(Engine &e, Surface *page, Args &a, int k) override {
		e_ = &e;
		fx::MoveContext mc;
		const char *str;
		if (sprite_) {
			Sprite *s = Object::as<Sprite>(layer_handle(a.u(k)));
			if (!s) return 1;
			target_ = s->handle();
			str = a.s(k + 1);
			dur_ = a.i(k + 2);
			if (!context(e, s, nullptr, page, mc)) return 1;
		} else {
			if (!ref_.attach(e, page)) return 1;
			str = a.s(k);
			dur_ = a.i(k + 1);
			if (!context(e, nullptr, page->comp(), page, mc)) return 2;
		}
		if (fx::parse_moves(str, mc, prog_) != 0) return 2;
		if (dur_ == 0) {
			apply_final();
			return -1;
		}
		return 0;
	}

	bool update(int64_t now) override {
		int loops = 0;
		for (;;) {
			if (idx_ >= prog_.steps.size()) return true;
			const fx::MoveStep *st = &prog_.steps[idx_];
			if (!started_) {
				fade_ = 0;
				while (st->op < 0) {
					switch (st->op) {
					case fx::MoveStep::kDuration: dur_ = st->value ? st->value : 1; break;
					case fx::MoveStep::kLoop:
						if (++loops > 1) return true;  // a loop without any move
						idx_ = 0;
						smooth_ = true;
						fade_ = 0;
						st = &prog_.steps[0];
						continue;
					case fx::MoveStep::kBehind: reorder(2); break;
					case fx::MoveStep::kFront: reorder(1); break;
					case fx::MoveStep::kFadeOut: fade_ = 2; break;
					case fx::MoveStep::kFadeIn: fade_ = 1; break;
					case fx::MoveStep::kEase: smooth_ = false, ease_ = st->ease; break;
					default: smooth_ = true; break;
					}
					if (++idx_ >= prog_.steps.size()) return true;
					st = &prog_.steps[idx_];
				}
				started_ = true;
				start_ = now;
				if (!position(sx_, sy_)) return true;
			}
			int64_t t = now - start_;
			if (t < 0) t = 0;
			if (uint64_t(t) >= uint64_t(int64_t(dur_) - 1)) {
				started_ = false;
				++idx_;
				t = dur_;
			}
			if (!smooth_) t = fx::ease_ms(t, dur_, ease_);
			int ox, oy;
			fx::move_offset(*st, t, dur_, smooth_, ox, oy);
			set_position(sx_ + ox, sy_ + oy);
			if (fade_ && sprite_) {
				int o = int(uint32_t(t * 255) / uint32_t(std::max(1, dur_)));
				if (fade_ == 2) o = 255 - o;
				if (Layer *l = layer()) l->set_opacity(o);
			}
			return false;
		}
	}

	void finish() override {
		if (idx_ < prog_.steps.size()) apply_final();
		idx_ = prog_.steps.size();
	}

	int query(Args &a, int k) override {
		a.out(k, prog_.final_x);
		a.out(k + 1, prog_.final_y);
		if (sprite_) {
			a.out(k + 2, prog_.final_opacity);
			a.out(k + 3, prog_.order);
		}
		return 0;
	}

	void apply_final() {
		set_position(prog_.final_x, prog_.final_y);
		if (!sprite_) return;
		if (prog_.final_opacity >= 0)
			if (Layer *l = layer()) l->set_opacity(prog_.final_opacity);
		if (prog_.order) reorder(prog_.order);
	}

	const fx::MoveProgram &program() const { return prog_; }

	static uint32_t layer_handle(uint32_t h) {
		Layer *l = layer_of(h);
		return l ? l->handle() : 0;
	}

private:
	Layer *layer() const { return Object::as<Layer>(target_); }

	bool position(int &x, int &y) {
		if (sprite_) {
			Layer *l = layer();
			if (!l) return false;
			x = l->x, y = l->y;
			return true;
		}
		Compositor *c = ref_.comp();
		if (!c) return false;
		x = c->bg_x, y = c->bg_y;
		return true;
	}

	void set_position(int x, int y) {
		if (sprite_) {
			if (Layer *l = layer()) l->set_pos(x, y);
			return;
		}
		Compositor *c = ref_.comp();
		if (!c || (c->bg_x == x && c->bg_y == y)) return;
		c->bg_x = x, c->bg_y = y;
		gfx_touch();
	}

	void reorder(int how) {
		Layer *l = layer();
		if (!l || !l->list()) return;
		l->list()->reprioritize(l, how == 1 ? g_front_prio : g_behind_prio, how == 1);
	}

	bool sprite_;
	Engine *e_ = nullptr;
	BgRef ref_;
	uint32_t target_ = 0;
	fx::MoveProgram prog_;
	size_t idx_ = 0;
	bool started_ = false, smooth_ = true;
	double ease_ = 0;
	int dur_ = 0, fade_ = 0, sx_ = 0, sy_ = 0;
	int64_t start_ = 0;
};

// ---------------------------------------------------------------- quake (layer)

class LayerQuake : public LayerEffect {
public:
	LayerQuake() : LayerEffect(kTaskOverlay) {}
	int init(Engine &e, Surface *page, Args &a, int k) override {
		if (!ref_.attach(e, page)) return 1;
		d_ = a.i(k);
		kind_ = a.i(k + 1);
		ax_ = a.i(k + 2), ay_ = a.i(k + 3);
		decay_ = fx::atof_or(a.s(k + 4), 0.0);
		bg_ = a.b(k + 6);
		fg_ = a.b(k + 7);
		if (!bg_ && !fg_) bg_ = fg_ = true;
		return 0;
	}
	bool update(int64_t now) override {
		if (start_ < 0) start_ = now;
		int64_t t = now - start_;
		int X = 0, Y = 0;
		if (d_ != -1 && t >= d_) {
			finish();
			return true;
		}
		double f = d_ == -1 ? 1.0 : 1.0 - (1.0 - decay_) * double(t) / std::max(1, d_);
		if (kind_ != 0) f *= double(d_ - t) / std::max(1, d_);  // damped variant [guess]
		auto centered = [&](int a) { return int(std::trunc((fx::rnd01() * a - a / 2) * f)); };
		if (ax_ == ay_) {
			X = centered(ax_), Y = centered(ay_);
		} else {
			double sg = sign_ ? -1.0 : 1.0;
			sign_ = !sign_;
			if (ax_ < ay_)
				X = centered(ax_), Y = int(std::trunc(sg * fx::rnd01() * ay_ * f));
			else
				X = int(std::trunc(sg * fx::rnd01() * ax_ * f)), Y = centered(ay_);
		}
		shift(X, Y);
		return false;
	}
	void finish() override { shift(0, 0); }

private:
	void shift(int X, int Y) {
		int dx = X - cur_x_, dy = Y - cur_y_;
		if (!dx && !dy) return;
		cur_x_ = X, cur_y_ = Y;
		Surface *pg = ref_.page();
		Compositor *c = pg ? pg->comp() : nullptr;
		if (!c) return;
		if (bg_) c->bg_x -= dx, c->bg_y -= dy;
		if (fg_)
			for (auto &n : c->main.nodes()) n.layer->set_pos(n.layer->x + dx, n.layer->y + dy);
		gfx_touch();
	}

	BgRef ref_;
	int d_ = 0, kind_ = 0, ax_ = 0, ay_ = 0;
	double decay_ = 0;
	bool bg_ = true, fg_ = true, sign_ = false;
	int cur_x_ = 0, cur_y_ = 0;
	int64_t start_ = -1;
};

// ---------------------------------------------------------------- overlay particles

// A page-sized canvas shown by one sprite on the front and/or back page at priority z.
struct Canvas {
	ImageRef img;
	std::unique_ptr<Sprite> spr[2];
	std::vector<Rect> dirty;
	void create(int W, int H, int z, Surface *page, bool front, bool back, int mode, int opacity) {
		img = std::make_shared<Image>(W, H, 32);
		img->has_alpha = true;
		Surface *pages[2] = {front ? page : nullptr, back && page ? page->linked : nullptr};
		for (int i = 0; i < 2; ++i) {
			if (!pages[i] || !pages[i]->comp()) continue;
			spr[i] = std::make_unique<Sprite>(img, nullptr, 0, 0);
			spr[i]->mode = mode;
			spr[i]->opacity = opacity;
			pages[i]->comp()->main.insert(spr[i].get(), z, 0, true);
		}
	}
	void clear() {
		for (const Rect &r : dirty) fill_rect(*img, r, 0);
		dirty.clear();
	}
	void mark(const Rect &r) {
		Rect c = r.intersect(img->rect());
		if (!c.empty()) dirty.push_back(c);
	}
};

class Particles : public LayerEffect {
public:
	enum Kind { kPaper, kLight, kSnow, kRain };
	explicit Particles(Kind k) : LayerEffect(kTaskOverlay), kind_(k) {}

	int init(Engine &e, Surface *page, Args &a, int k) override {
		page_ = page;
		W_ = page->image().width(), H_ = page->image().height();
		int count = std::clamp(a.i(k), 0, 1000);
		int zbase = a.i(k + 1), zstep = a.i(k + 2);
		bool prewarm = a.b(k + 3), front = a.b(k + 4), back = a.b(k + 5);
		if (!front && !back) front = true;
		int classes = 1;
		switch (kind_) {
		case kPaper:
			classes = 8;
			for (int i = 0; i < 8; ++i) imgs_.push_back(e.load_image("SYS\\PAPER_" + std::to_string(i) + ".ELG", false));
			break;
		case kLight:
			classes = 3;
			for (int i = 0; i < 3; ++i) imgs_.push_back(e.load_image("SYS\\GR_LT" + std::to_string(i) + ".ELG", false));
			break;
		case kSnow:
			classes = 5;
			for (int i = 0; i < 5; ++i) imgs_.push_back(e.load_image("SYS\\SNOW_" + std::to_string(i) + ".ELG", false));
			break;
		case kRain: classes = 6; break;
		}
		for (auto &im : imgs_)
			if (!im) log_warn("%s: particle image missing", name());
		canvases_.resize(size_t(classes));
		for (int c = 0; c < classes; ++c) {
			int z = kind_ == kPaper ? zbase + (4 - c) * zstep : kind_ == kRain ? zbase + (2 - c) * zstep
			                                                                    : zbase + (classes / 2 - c) * zstep;
			canvases_[size_t(c)].create(W_, H_, z, page, front, back, kBlendAlpha, 255);
		}
		parts_.resize(size_t(count));
		for (auto &p : parts_) {
			spawn(p);
			if (prewarm) p.y = fx::rnd01() * H_;
			else p.y -= fx::rnd01() * H_;  // stagger the first entries
		}
		return 0;
	}

	bool update(int64_t now) override {
		if (last_ < 0) last_ = now;
		double dt = double(now - last_) / 33.0;  // the original advances per ~33 ms tick
		last_ = now;
		if (dt > 4) dt = 4;
		bool any = false;
		for (auto &c : canvases_) c.clear();
		for (auto &p : parts_) {
			if (!p.alive) continue;
			step(p, dt);
			if (p.y > H_ + 40 || p.y < -200 || p.x < -100 || p.x > W_ + 100) {
				if (stopping_) {
					p.alive = false;
					continue;
				}
				spawn(p);
			}
			any = true;
			draw(p);
		}
		gfx_touch();
		return stopping_ && !any;
	}

	int control(Args &, int) override {
		stopping_ = true;
		return 0;
	}

private:
	struct P {
		double x = 0, y = 0, vx = 0, vy = 0, ax = 0, phase = 0;
		int k = 0, frame = 0;
		double ftime = 0;
		bool alive = true;
	};

	const char *name() const {
		return kind_ == kPaper ? "paper" : kind_ == kLight ? "light" : kind_ == kSnow ? "snow" : "rain";
	}

	void spawn(P &p) {
		p.alive = true;
		p.x = fx::rnd01() * (W_ + 100) - 50;
		p.y = -20 - fx::rnd01() * 40;
		p.phase = fx::rnd01() * 6.28;
		switch (kind_) {
		case kSnow:
			p.k = int(fx::rnd() % 5);
			p.vy = 0.6 * fx::rnd01() + 0.5 * p.k + 1.2;
			p.vx = 0, p.ax = 0;
			break;
		case kRain:
			p.k = int(fx::rnd() % 6);
			p.vy = 14 + 3.0 * p.k + 4 * fx::rnd01();
			p.vx = -1.5 - 0.3 * p.k;
			break;
		case kPaper:
			p.k = int(fx::rnd() & 7);
			p.vy = 0.8 + 0.4 * fx::rnd01() + (p.k >= 4 ? 0.6 : 0);
			p.vx = (fx::rnd01() - 0.5) * 2;
			p.frame = int(fx::rnd() % 24);
			break;
		case kLight:
			p.k = int(fx::rnd() % 3);
			p.y = H_ + 20 + fx::rnd01() * 40;
			p.vy = -(0.3 + 0.3 * fx::rnd01() + 0.2 * p.k);
			p.vx = (fx::rnd01() - 0.5) * 0.6;
			p.frame = int(fx::rnd() % 5);
			break;
		}
	}

	void step(P &p, double dt) {
		switch (kind_) {
		case kSnow:
		case kPaper:
			// random-walk sway, horizontal speed clamped to ±1.5
			p.ax += (fx::rnd01() - 0.5) * 0.05 * dt;
			p.ax = std::clamp(p.ax, -0.1, 0.1);
			p.vx = std::clamp(p.vx + p.ax * dt, -1.5, 1.5);
			break;
		default: break;
		}
		p.x += p.vx * dt;
		p.y += p.vy * dt;
		p.ftime += dt;
		if (kind_ == kPaper && p.ftime >= 1.0) p.frame = (p.frame + int(p.ftime)) % 24, p.ftime = 0;
		if (kind_ == kLight && p.ftime >= 3.0) p.frame = (p.frame + 1) % 5, p.ftime = 0;
	}

	void draw(const P &p) {
		Canvas &cv = canvases_[size_t(p.k) % canvases_.size()];
		int x = int(p.x), y = int(p.y);
		if (kind_ == kRain) {
			int len = 18 + p.k * 8;
			uint32_t alpha = uint32_t(70 + p.k * 25);
			double sx = p.vx / p.vy;
			for (int i = 0; i < len; ++i) {
				int px = x + int(sx * i), py = y + i;
				if (px < 0 || py < 0 || px >= W_ || py >= H_) continue;
				uint32_t a = alpha * uint32_t(i + 1) / uint32_t(len);
				cv.img->row32(py)[px] = (a << 24) | 0xe0e8f0;
			}
			int x0 = std::min(x, x + int(sx * len)), x1 = std::max(x, x + int(sx * len)) + 1;
			cv.mark(Rect(x0, y, x1, y + len));
			return;
		}
		ImageRef im = imgs_.empty() ? nullptr : imgs_[size_t(p.k) % imgs_.size()];
		if (!im) return;
		Rect sr = im->rect();
		if (kind_ == kPaper) {
			int fw = im->width() / 24 > 0 ? im->width() / 24 : im->width();
			sr = Rect(p.frame * fw, 0, p.frame * fw + fw, im->height());
			if (sr.r > im->width()) sr = Rect(0, 0, fw, im->height());
		} else if (kind_ == kLight) {
			int fw = im->width() / 5;
			sr = Rect(p.frame * fw, 0, p.frame * fw + fw, im->height());
		}
		blit(*cv.img, *im, x, y, sr, kBlendCopy, 255, false);
		cv.mark(Rect(x, y, x + sr.w(), y + sr.h()));
	}

	Kind kind_;
	Surface *page_ = nullptr;
	int W_ = 0, H_ = 0;
	std::vector<ImageRef> imgs_;
	std::vector<Canvas> canvases_;
	std::vector<P> parts_;
	int64_t last_ = -1;
	bool stopping_ = false;
};

class Noise : public LayerEffect {
public:
	Noise() : LayerEffect(kTaskOverlay) {}
	int init(Engine &e, Surface *page, Args &a, int k) override {
		noise_ = e.load_image("SYS\\NOISE.ELG", true);
		if (!noise_) return 2;
		int opa = a.i(k), z = a.i(k + 1);
		bool front = a.b(k + 2), back = a.b(k + 3);
		if (!front && !back) front = true;
		cv_.create(page->image().width(), page->image().height(), z, page, front, back, kBlendCopy, opa);
		cv_.img->has_alpha = false;
		interval = 100;
		refresh();
		return 0;
	}
	bool update(int64_t) override {
		refresh();
		return false;
	}

private:
	void refresh() {
		Image &d = *cv_.img;
		const Image &n = *noise_;
		size_t total = size_t(n.width()) * n.height();
		if (!total) return;
		size_t off = fx::rnd() % total;
		for (int y = 0; y < d.height(); ++y) {
			uint32_t *row = d.row32(y);
			for (int x = 0; x < d.width(); ++x) {
				size_t i = (off + size_t(y) * d.width() + x) % total;
				uint32_t v = n.pixel(int(i % n.width()), int(i / n.width()));
				row[x] = v | 0xff000000u;
			}
		}
		gfx_touch();
	}
	ImageRef noise_;
	Canvas cv_;
};

// ---------------------------------------------------------------- bg_anime (not implemented)

class BgAnime : public LayerEffect {
public:
	BgAnime() : LayerEffect(kTaskBgAnime) {}
	int init(Engine &, Surface *, Args &, int) override {
		static bool warned = false;
		if (!warned) {
			warned = true;
			log_warn("bg_anime: not implemented");
		}
		return -1;
	}
	bool update(int64_t) override { return true; }
};

// ---------------------------------------------------------------- N_35: zoom pulse

// Second animator class (types 12001/12005/12006): N_35(sprite, mode, ms, scaleStr, easeStr)
// animates the sprite size through the keys (w·scale, h·scale), (w, h) with two segments of
// ms/2 (FUN_0041efe0). The size is applied about the sprite centre [guess: the anchor
// selected by the class's kind byte 1/2 is not traced].
class ZoomPulse : public Task {
public:
	ZoomPulse(Sprite *s, int type, int ms, double scale, double ease, bool hold)
	    : Task(type), target_(s->handle()), ms_(std::max(2, ms)), scale_(scale), ease_(ease), hold_(hold) {
		w0_ = s->zoom_w ? s->zoom_w : s->src.w();
		h0_ = s->zoom_h ? s->zoom_h : s->src.h();
		cx_ = s->x * 2 + w0_, cy_ = s->y * 2 + h0_;  // doubled centre
	}
	bool update(int64_t now) override {
		if (start_ < 0) start_ = now;
		int64_t t = now - start_;
		if (t >= ms_) {
			finish();
			return true;
		}
		int64_t half = ms_ / 2;
		double p;
		if (hold_) {
			p = double(fx::ease_ms(t, ms_, ease_)) / ms_;
		} else {
			int64_t tt = t < half ? t : ms_ - t;
			p = double(fx::ease_ms(tt, half, ease_)) / std::max<int64_t>(1, half);
		}
		apply(1.0 + (scale_ - 1.0) * p);
		return false;
	}
	void finish() override { apply(hold_ ? scale_ : 1.0); }

private:
	void apply(double z) {
		Sprite *s = Object::as<Sprite>(target_);
		if (!s) return;
		int w = int(std::trunc(w0_ * z)), h = int(std::trunc(h0_ * z));
		s->zoom_w = (w == s->src.w()) ? 0 : w;
		s->zoom_h = (h == s->src.h()) ? 0 : h;
		s->set_pos((cx_ - w) / 2, (cy_ - h) / 2);
	}
	uint32_t target_;
	int ms_;
	double scale_, ease_;
	bool hold_;
	int w0_, h0_, cx_, cy_;
	int64_t start_ = -1;
};

// ---------------------------------------------------------------- registry

std::unique_ptr<LayerEffect> make_effect(const std::string &n) {
	if (n == "bg_rast") return std::make_unique<BgWave>(false);
	if (n == "bg_haze") return std::make_unique<BgWave>(true);
	if (n == "bg_zoom") return std::make_unique<BgZoom>();
	if (n == "bg_rot") return std::make_unique<BgRot>();
	if (n == "bg_blur") return std::make_unique<BgBlur>();
	if (n == "bg_move") return std::make_unique<MoveTask>(false);
	if (n == "sp_move") return std::make_unique<MoveTask>(true);
	if (n == "quake") return std::make_unique<LayerQuake>();
	if (n == "noise") return std::make_unique<Noise>();
	if (n == "paper") return std::make_unique<Particles>(Particles::kPaper);
	if (n == "light") return std::make_unique<Particles>(Particles::kLight);
	if (n == "snow") return std::make_unique<Particles>(Particles::kSnow);
	if (n == "rain") return std::make_unique<Particles>(Particles::kRain);
	if (n == "bg_anime") return std::make_unique<BgAnime>();
	return nullptr;
}

}  // namespace

void register_layer_effect_natives(Engine &e) {
	auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		e.vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};

	def(0xd6, "effect_create", [&e](Args &a) -> int32_t {
		Surface *page = e.surface_of(a.u(0));
		if (!page) return 0;
		std::string name = a.s(1);
		auto fxo = make_effect(name);
		if (!fxo) {
			log_warn("N_d6: unknown effect '%s'", name.c_str());
			return 0;
		}
		int r = fxo->init(e, page, a, 2);
		if (r != 0) {
			if (r > 0) log_warn("N_d6: %s init failed (%d)", name.c_str(), r);
			return 0;
		}
		LayerEffect *p = fxo.get();
		int64_t iv = p->interval;
		e.tasks.add(std::move(fxo), iv, 0);
		return int32_t(p->handle());
	});
	def(0xd7, "effect_control", [](Args &a) -> int32_t {
		LayerEffect *fxo = Object::as<LayerEffect>(a.u(0));
		return fxo ? fxo->control(a, 1) : -1;
	});
	def(0xd8, "effect_query", [](Args &a) -> int32_t {
		LayerEffect *fxo = Object::as<LayerEffect>(a.u(0));
		return fxo ? fxo->query(a, 1) : -1;
	});
	def(0xd9, "effect_static", [&e](Args &a) -> int32_t {
		std::string name = a.s(0);
		if (name == "sp_move") {
			if (a.i(1) == 0) {
				g_front_prio = a.i(2);
				g_behind_prio = a.i(3);
				return 0;
			}
			Sprite *s = Object::as<Sprite>(MoveTask::layer_handle(a.u(2)));
			if (!s) return 1;
			fx::MoveContext mc;
			MoveTask::context(e, s, nullptr, nullptr, mc);
			fx::MoveProgram prog;
			int r = fx::parse_moves(a.s(3), mc, prog);
			if (r) return r;
			s->set_pos(prog.final_x, prog.final_y);
			a.out(4, prog.final_x);
			a.out(5, prog.final_y);
			a.out(6, prog.final_opacity);
			a.out(7, prog.order);
			return 0;
		}
		Surface *page = e.surface_of(a.u(1));
		Compositor *c = page ? page->comp() : nullptr;
		if (!c) return -1;
		if (name == "bg_move") {
			fx::MoveContext mc;
			if (!MoveTask::context(e, nullptr, c, page, mc)) return c->bg_cache ? 2 : 3;
			fx::MoveProgram prog;
			int r = fx::parse_moves(a.s(2), mc, prog);
			if (r) return r;
			if (c->bg_x != prog.final_x || c->bg_y != prog.final_y) {
				c->bg_x = prog.final_x, c->bg_y = prog.final_y;
				page->invalidate();
			}
			a.out(3, prog.final_x);
			a.out(4, prog.final_y);
			return 0;
		}
		if (name == "bg_zoom") {
			if (!c->bg) return 3;
			int W = page->image().width(), H = page->image().height();
			Rect r = zoom_target(c, W, H, a.i(2), a.i(3), fx::atof_or(a.s(4), 1.0));
			apply_view(c, W, H, r);
			a.out(5, r.l), a.out(6, r.t), a.out(7, r.w()), a.out(8, r.h());
			return 0;
		}
		if (name == "bg_blur") {
			// final state: the zoom view (angle 0) or a rotation (FUN_0043f250 approximated)
			if (!c->bg) return 3;
			int W = page->image().width(), H = page->image().height();
			double z = fx::atof_or(a.s(4), 1.0), ang = fx::atof_or(a.s(5), 0.0);
			if (ang != 0.0) {
				int ai = int(std::trunc(ang * -512.0));
				apply_rot(c, W, H, a.i(2), a.i(3), ai, z);
				a.out(6, a.i(2)), a.out(7, a.i(3)), a.out(8, ai), a.out(9, 0);
				return 0;
			}
			Rect r = zoom_target(c, W, H, a.i(2), a.i(3), z);
			apply_view(c, W, H, r);
			a.out(6, r.l), a.out(7, r.t), a.out(8, r.w()), a.out(9, r.h());
			return 0;
		}
		log_warn("N_d9: '%s' not implemented", name.c_str());
		return -1;
	});

	def(0x35, "sprite_zoom_pulse", [&e](Args &a) -> int32_t {
		Sprite *s = Object::as<Sprite>(MoveTask::layer_handle(a.u(0)));
		int mode = int(a.u(1) & 0xff);
		if (!s || mode >= 2) return 0;
		double scale = fx::atof_or(a.s(3), 1.0), ease = fx::atof_or(a.s(4), 0.0);
		int type = 12001;
		if (s->list() && s->list()->owner()) type = s->list() == &s->list()->owner()->main ? 12005 : 12006;
		auto t = std::make_unique<ZoomPulse>(s, type, a.i(2), scale, ease, false);
		Task *p = t.get();
		e.tasks.add(std::move(t));
		return int32_t(p->handle());
	});
}

}  // namespace ore
