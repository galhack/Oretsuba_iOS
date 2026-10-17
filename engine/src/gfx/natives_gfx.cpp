// Graphics-core natives N_c5..N_10f, N_18d..N_1c7 (docs/re/20_graphics_core.md §5).
#include <cmath>
#include <cstring>

#include "core/args.h"
#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/path_anim.h"
#include "ui/ui_system.h"
#include "util/log.h"

namespace ore {
namespace {

// A rect argument group: (l, t, r, b) or 0xffff in the first word = whole image.
bool rect_arg(Args &a, int i, Rect &out) {
	if (a.u(i) == 0xffff) return false;
	out = Rect(a.i(i), a.i(i + 1), a.i(i + 2), a.i(i + 3));
	return true;
}

std::vector<PathAnimator::Key> parse_keys(const char *s) {
	std::vector<PathAnimator::Key> keys;
	const char *p = s;
	while ((p = strchr(p, '('))) {
		++p;
		double v[3] = {0, 0, 255};
		for (int k = 0; k < 3; ++k) {
			v[k] = atof(p);
			const char *c = strpbrk(p, ",)");
			if (!c) break;
			p = c + 1;
			if (*c == ')') break;
		}
		keys.push_back({v[0], v[1], int(v[2])});
	}
	return keys;
}

}  // namespace

// Resolves any layer-like handle (sprites, colour rects, a control's sprite).
Layer *layer_of(uint32_t h) {
	if (Layer *l = Object::as<Layer>(h)) return l;
	if (UiControl *c = Object::as<UiControl>(h)) return c->main_layer();
	return nullptr;
}

void register_gfx_natives(Engine &e) {
	auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		e.vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};

	// ---- screen registry, images, pages
	def(0xc5, "image_tag", [&e](Args &a) {
		int old = e.image_tag;
		e.image_tag = a.i(0);
		return old;
	});
	def(0xc6, "delete_by_tag", [&e](Args &a) {
		e.delete_images_by_tag(a.i(0), a.i(1));
		return 0;
	});
	def(0xc8, "cache_release", [](Args &) { return 0; });
	def(0xcd, "present_rect_flag", [](Args &) { return 0; });
	def(0xce, "present_rect", [](Args &) { return 0; });
	def(0xcf, "task_alive", [&e](Args &a) {
		Task *t = Object::as<Task>(a.u(0));
		return t && e.tasks.contains(t) && !t->killed ? 1 : 0;
	});
	def(0xd0, "task_kill", [&e](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 0;
		uint32_t h = e.vm.mem().rd32(p);
		e.vm.mem().wr32(p, 0);
		Task *t = Object::as<Task>(h);
		return t && e.tasks.kill(t) ? 1 : 0;
	});
	def(0xd1, "task_kill_type", [&e](Args &a) {
		e.tasks.kill_type(a.i(0));
		return 0;
	});
	def(0xd2, "task_pause", [&e](Args &a) {
		if (Task *t = Object::as<Task>(a.u(0))) e.tasks.pause(t);
		return 0;
	});
	def(0xd3, "task_pause_type", [&e](Args &a) {
		e.tasks.pause_type(a.i(0));
		return 0;
	});
	def(0xd4, "task_resume", [&e](Args &a) {
		if (Task *t = Object::as<Task>(a.u(0))) e.tasks.resume(t);
		return 0;
	});
	def(0xd5, "task_resume_type", [&e](Args &a) {
		e.tasks.resume_type(a.i(0));
		return 0;
	});
	def(0xde, "new_screen_page", [&e](Args &a) -> int32_t {
		auto s = std::make_unique<Surface>(7, a.i(0), a.i(1), a.i(2), a.i(3));
		s->image().tag = e.image_tag;
		return int32_t(e.own_image(std::move(s))->handle());
	});
	def(0xe3, "new_image", [&e](Args &a) -> int32_t {
		int bpp = a.i(2);
		if (bpp != 8 && bpp != 32) return 0;
		auto img = std::make_shared<Image>(a.i(0), a.i(1), bpp);
		img->tag = e.image_tag;
		return int32_t(e.own_image(std::make_unique<ImageObj>(img))->handle());
	});
	auto load = [&e](Args &a, bool keep8) -> int32_t {
		ImageRef img = e.load_image(a.s(0), keep8);
		if (!img) return 0;
		img->tag = e.image_tag;
		return int32_t(e.own_image(std::make_unique<ImageObj>(img))->handle());
	};
	def(0xe4, "load_image", [load](Args &a) { return load(a, false); });
	def(0xe5, "load_image8", [load](Args &a) { return load(a, true); });
	def(0xe9, "clone_image", [&e](Args &a) -> int32_t {
		ImageRef src = e.image_of(a.u(0));
		if (!src) return 0;
		ImageRef c = src->clone();
		c->tag = e.image_tag;
		return int32_t(e.own_image(std::make_unique<ImageObj>(c))->handle());
	});
	def(0xea, "new_gdi_page", [&e](Args &a) -> int32_t {
		auto s = std::make_unique<Surface>(9, a.i(0), a.i(1), a.i(2), a.i(3));
		s->image().tag = e.image_tag;
		return int32_t(e.own_image(std::move(s))->handle());
	});
	def(0xeb, "new_back_page", [&e](Args &a) -> int32_t {
		auto s = std::make_unique<Surface>(10, a.i(0), a.i(1), a.i(2), a.i(3));
		s->image().tag = e.image_tag;
		return int32_t(e.own_image(std::move(s))->handle());
	});
	def(0xed, "main_object", [](Args &) { return 0; });  // DirectDraw path: views
	def(0xee, "set_main_object", [&e](Args &a) {
		e.main_object = e.surface_of(a.u(0));
		gfx_touch();
		return 0;
	});
	def(0xef, "delete_image", [&e](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 0;
		uint32_t h = e.vm.mem().rd32(p);
		if (e.delete_image(h)) e.vm.mem().wr32(p, 0);
		return 0;
	});
	def(0xf0, "width", [&e](Args &a) {
		ImageRef i = e.image_of(a.u(0));
		return i ? i->width() : 0;
	});
	def(0xf1, "height", [&e](Args &a) {
		ImageRef i = e.image_of(a.u(0));
		return i ? i->height() : 0;
	});
	def(0xf2, "key_from_pixel", [&e](Args &a) {
		ImageRef i = e.image_of(a.u(0));
		if (!i) return 0;
		int x = a.i(1), y = a.i(2);
		if (x < 0 || y < 0 || x >= i->width() || y >= i->height())
			i->color_key = 0xFFFFFFFF;
		else
			i->color_key = i->bpp() == 8 ? i->row8(y)[x] : i->row32(y)[x];
		gfx_touch();
		return i->color_key == 0xFFFFFFFF ? 1 : 0;
	});
	def(0xf5, "offset_x", [&e](Args &a) {
		ImageRef i = e.image_of(a.u(0));
		return i ? i->offset_x : 0;
	});
	def(0xf6, "offset_y", [&e](Args &a) {
		ImageRef i = e.image_of(a.u(0));
		return i ? i->offset_y : 0;
	});
	def(0xf7, "fill_image", [&e](Args &a) {
		ImageRef i = e.image_of(a.u(0));
		if (!i) return -1;
		i->fill((uint32_t(a.u(1) & 0xff) << 16) | (uint32_t(a.u(2) & 0xff) << 8) | (a.u(3) & 0xff));
		gfx_touch();
		return 0;
	});
	def(0xf9, "link_pages", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s) return -1;
		return s->link(e.surface_of(a.u(1))) ? 0 : 1;
	});
	def(0xfa, "page_action", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->linked) return -1;
		int m = a.i(1);
		if (m != 1) s->swap_compositors(*s->linked);
		if (m == 1 || m == 2) s->comp()->swap_background(*s->linked->comp());
		s->invalidate();
		return 1;
	});
	def(0xfc, "end_transition", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s) return -1;
		s->end_transition(s->trans_apply);
		return 1;
	});
	def(0xfd, "transition_active", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		return s && s->transition_active() ? 1 : 0;
	});
	def(0xfe, "transition_work", [&e](Args &a) -> int32_t {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->work) return 0;
		return int32_t(e.own_image(std::make_unique<ImageObj>(s->work))->handle());
	});
	def(0x106, "new_view", [&e](Args &a) -> int32_t {
		Surface *s = e.surface_of(a.u(0));
		if (!s) return 0;
		Rect r;
		bool has = rect_arg(a, 1, r);
		int x = has ? a.i(5) : 0, y = has ? a.i(6) : 0;
		auto v = std::make_unique<View>(s, has ? &r : nullptr, x, y);
		v->tag = e.image_tag;
		View *p = v.get();
		e.views.push_back(std::move(v));
		return int32_t(p->handle());
	});
	def(0x10a, "insert_view", [&e](Args &a) {
		View *v = Object::as<View>(a.u(0));
		if (!v) return 1;
		e.insert_view(v, a.i(1));
		return 0;
	});
	def(0x10f, "set_visible_obj", [&e](Args &a) {
		if (View *v = Object::as<View>(a.u(0))) {
			v->visible = a.b(1);
			gfx_touch();
			return 1;
		}
		if (Layer *l = layer_of(a.u(0))) {
			l->set_visible(a.b(1));
			return 1;
		}
		(void)e;
		return 0;
	});

	// ---- sprites and compositor
	def(0x18d, "new_sprite", [&e](Args &a) -> int32_t {
		ImageRef img = e.image_of(a.u(0));
		if (!img) return 0;
		Rect r;
		bool has = rect_arg(a, 1, r);
		int x = has ? a.i(5) : 0, y = has ? a.i(6) : 0;  // 0xffff form: placed at 0,0
		auto s = std::make_unique<Sprite>(img, has ? &r : nullptr, x, y);
		s->tag = e.sprite_tag;
		return int32_t(e.own_layer(std::move(s))->handle());
	});
	def(0x18e, "clone_sprite", [&e](Args &a) -> int32_t {
		Sprite *s = Object::as<Sprite>(a.u(0));
		if (!s) return 0;
		auto c = s->clone();
		return int32_t(e.own_layer(std::move(c))->handle());
	});
	def(0x18f, "new_color_rect", [&e](Args &a) -> int32_t {
		auto r = std::make_unique<ColorRect>(a.i(0), a.i(1), a.u(2), a.i(3), a.i(4));
		r->tag = e.sprite_tag;
		return int32_t(e.own_layer(std::move(r))->handle());
	});
	def(0x191, "sprite_tag", [&e](Args &a) {
		int old = e.sprite_tag;
		e.sprite_tag = a.i(0);
		return old;
	});
	def(0x192, "delete_sprites_by_tag", [&e](Args &a) {
		e.delete_layers_by_tag(a.i(0), a.b(1));
		return 0;
	});
	def(0x193, "delete_sprite", [&e](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 0;
		if (e.delete_layer(e.vm.mem().rd32(p))) e.vm.mem().wr32(p, 0);
		return 0;
	});
	def(0x194, "add_to_page", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp()) return -1;
		Layer *l = layer_of(a.u(1));
		if (!l) return 1;
		if (l->list()) return 2;
		int prio = a.i(2), tag = a.i(3);
		LayerList *list = &s->comp()->main;
		if (prio == -1) {
			prio = 0;
			tag = 0;
		} else if (a.b(4)) {
			list = s->overlay_list();
			if (!list) return 4;
		}
		list->insert(l, prio, tag, true);
		return 0;
	});
	auto reprio = [&e](Args &a, bool before) {
		Layer *l = layer_of(a.u(1));
		if (!l || !l->list()) return 0;
		LayerList::Node *n = l->list()->find(l);
		if (a.b(3) && n && n->prio == a.i(2)) return 1;
		l->list()->reprioritize(l, a.i(2), before);
		(void)e;
		return 1;
	};
	def(0x199, "reprioritize_front", [reprio](Args &a) { return reprio(a, true); });
	def(0x19a, "reprioritize_back", [reprio](Args &a) { return reprio(a, false); });
	def(0x19d, "set_background", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp()) return 0;
		Compositor *c = s->comp();
		c->bg = a.u(1) ? e.image_of(a.u(1)) : nullptr;
		c->bg_x = a.i(2);
		c->bg_y = a.i(3);
		c->bg_cache.reset();
		if (c->bg) {
			c->rot_cx = c->bg->width() / 2;
			c->rot_cy = c->bg->height() / 2;
		}
		s->invalidate();
		return 1;
	});
	def(0x19e, "set_background_color", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp()) return 0;
		Compositor *c = s->comp();
		int mode = a.i(2);
		if (mode < 2) {
			c->bg.reset();
			c->bg_cache.reset();
		}
		c->bg_color = a.u(1) & 0xffffff;
		s->invalidate();
		return 1;
	});
	def(0x19f, "set_background_offset", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp()) return 0;
		Compositor *c = s->comp();
		if (c->bg_x == a.i(1) && c->bg_y == a.i(2)) return 0;
		c->bg_x = a.i(1);
		c->bg_y = a.i(2);
		s->invalidate();
		return 1;
	});
	def(0x1a0, "zoom_background", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp() || !s->comp()->bg) return 0;
		Compositor *c = s->comp();
		int x = a.i(1), y = a.i(2), w = a.i(3), h = a.i(4);
		Rect sr = c->bg->rect();
		rect_arg(a, 5, sr);
		if (w <= 0) w = c->bg->width();
		if (h <= 0) h = c->bg->height();
		if (x == 0 && y == 0 && w == sr.w() && h == sr.h() && sr == c->bg->rect()) {
			c->bg_cache.reset();
		} else {
			Image &pg = s->image();
			auto cache = std::make_shared<Image>(pg.width(), pg.height(), 32);
			fill_rect(*cache, cache->rect(), c->bg_color);
			stretch_blit(*cache, *c->bg, x, y, w, h, sr, 0, 0, 255, false);
			c->bg_cache = cache;
		}
		s->invalidate();
		return 1;
	});
	def(0x1a2, "rotate_background", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp() || !s->comp()->bg) return 0;
		Compositor *c = s->comp();
		int x = a.i(1), y = a.i(2), angle = a.i(3);
		float zf;
		uint32_t zb = a.u(4);
		memcpy(&zf, &zb, 4);
		double zoom = zf;
		if (angle == 0 && std::fabs(zoom - 1.0) < 1e-9 && x == 0 && y == 0) {
			c->bg_cache.reset();
			s->invalidate();
			return 1;
		}
		Image &pg = s->image();
		auto cache = std::make_shared<Image>(pg.width(), pg.height(), 32);
		fill_rect(*cache, cache->rect(), c->bg_color);
		double th = angle * 2.0 * M_PI / 512.0, co = std::cos(th), si = std::sin(th);
		double cx = c->rot_cx * zoom, cy = c->rot_cy * zoom;
		// rotate the zoomed quad about (cx, cy); then move its bounding box to (x, y)
		double W = c->bg->width() * zoom, H = c->bg->height() * zoom;
		double px[4] = {0, W, 0, W}, py[4] = {0, 0, H, H};
		double minx = 1e18, miny = 1e18;
		for (int k = 0; k < 4; ++k) {
			double dx = px[k] - cx, dy = py[k] - cy;
			double X = co * dx + si * dy + cx, Y = -si * dx + co * dy + cy;
			minx = std::min(minx, X), miny = std::min(miny, Y);
		}
		double A = co * zoom, B = si * zoom, C = -si * zoom, D = co * zoom;
		double tx = -co * cx - si * cy + cx - minx + x, ty = si * cx - co * cy + cy - miny + y;
		affine_blit(*cache, *c->bg, c->bg->rect(), A, B, C, D, tx, ty, 0, 255, false);
		c->bg_cache = cache;
		s->invalidate();
		return 1;
	});
	def(0x1a3, "background_state", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp()) return 0;
		Compositor *c = s->comp();
		a.out(1, 0);  // background object handles are not exported
		a.out(2, c->bg_x);
		a.out(3, c->bg_y);
		a.out(4, int(c->bg_color & 0xff));
		a.out(5, 0);
		a.out(6, int(c->bg_color));
		return 1;
	});
	def(0x1a4, "invalidate_page", [&e](Args &a) {
		if (Surface *s = e.surface_of(a.u(0))) s->invalidate();
		return 0;
	});
	def(0x1a6, "page_compose_enable", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s) return 0;
		bool old = s->use_compositor;
		s->use_compositor = a.b(1) && s->comp();
		s->invalidate();
		return old ? 1 : 0;
	});
	def(0x1a8, "snapshot_page", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		ImageRef dst = e.image_of(a.u(1));
		if (!s || !s->comp()) return -1;
		if (!dst) return 1;
		if (dst->width() != s->image().width() || dst->height() != s->image().height()) return 2;
		s->comp()->compose(*dst, a.i(2), a.i(3), a.b(4));
		gfx_touch();
		return 0;
	});
	def(0x1a9, "page_hide_lists", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp()) return 0;
		s->comp()->hide_main = a.b(1);
		s->comp()->hide_overlay = a.b(2);
		s->invalidate();
		return 1;
	});
	def(0x1aa, "page_no_background", [&e](Args &a) {
		Surface *s = e.surface_of(a.u(0));
		if (!s || !s->comp()) return 0;
		s->comp()->no_bg = a.b(1);
		s->invalidate();
		return 1;
	});

	// ---- generic layer operations
	def(0x1ab, "banded_compose", [](Args &) { return 0; });
	def(0x1ac, "set_visible", [](Args &a) {
		if (Layer *l = layer_of(a.u(0))) l->set_visible(a.b(1));
		return 0;
	});
	def(0x1ad, "list_state", [](Args &a) {
		Layer *l = layer_of(a.u(0));
		if (!l || !l->list()) return 0;
		return l->visible ? 2 : 1;
	});
	def(0x1ae, "mark_dirty", [](Args &a) {
		if (Layer *l = layer_of(a.u(0))) l->touch();
		return 1;
	});
	def(0x1b0, "set_blend_mode", [](Args &a) {
		Layer *l = layer_of(a.u(0));
		if (!l) return -2;
		return l->set_mode(a.i(1)) ? 1 : 0;
	});
	def(0x1b1, "set_opacity", [](Args &a) {
		Layer *l = layer_of(a.u(0));
		if (!l) return -2;
		l->set_opacity(a.i(1));
		return 1;
	});
	def(0x1b2, "set_position", [](Args &a) {
		Layer *l = layer_of(a.u(0));
		if (!l) return 0;
		bool changed = l->x != a.i(1) || l->y != a.i(2);
		l->set_pos(a.i(1), a.i(2));
		return changed ? 1 : 0;
	});
	def(0x1b3, "move_by", [](Args &a) {
		Layer *l = layer_of(a.u(0));
		if (!l) return 0;
		l->set_pos(l->x + a.i(1), l->y + a.i(2));
		return 1;
	});
	def(0x1b4, "hit_test", [](Args &a) {
		Layer *l = layer_of(a.u(0));
		return l && l->hit(a.i(1), a.i(2), a.b(3)) ? 1 : 0;
	});
	def(0x1b6, "bounds", [](Args &a) {
		Layer *l = layer_of(a.u(0));
		if (!l) return 0;
		Rect r = l->bounds();
		a.out(1, r.l), a.out(2, r.t), a.out(3, r.r), a.out(4, r.b);
		return 1;
	});
	auto animate = [&e](Layer *l, int ms, std::vector<PathAnimator::Key> keys, Args &a, int k) -> int32_t {
		if (!l) return 0;
		int delay = a.i(k), temp = a.i(k + 1);
		int what = 1;
		bool spline = false, loop = false;
		double ease = 0;
		if (a.u(k + 2) != 0xffff) {
			what = a.i(k + 2);
			spline = a.b(k + 3);
			ease = a.null(k + 4) ? 0.0 : atof(a.s(k + 4));
			loop = a.b(k + 5);
		}
		auto t = std::make_unique<PathAnimator>(l, ms, std::move(keys), delay, temp != 0, what, spline, ease, loop);
		Task *p = t.get();
		e.tasks.add(std::move(t));
		return int32_t(p->handle());
	};
	def(0x1b8, "animate_path", [animate](Args &a) {
		return animate(layer_of(a.u(0)), a.i(1), parse_keys(a.s(2)), a, 3);
	});
	def(0x1b9, "animate_path_array", [&e, animate](Args &a) {
		int n = a.i(2);
		std::vector<PathAnimator::Key> keys;
		uint32_t arr = a.u(3);
		for (int i = 0; i < n && arr; ++i) {
			keys.push_back({double(int32_t(e.vm.mem().rd32(arr + uint32_t(i) * 12))),
			                double(int32_t(e.vm.mem().rd32(arr + uint32_t(i) * 12 + 4))),
			                int(int32_t(e.vm.mem().rd32(arr + uint32_t(i) * 12 + 8)))});
		}
		return animate(layer_of(a.u(0)), a.i(1), std::move(keys), a, 4);
	});
	def(0x1c1, "sprite_scroll", [](Args &a) {
		Sprite *s = Object::as<Sprite>(a.u(0));
		if (!s) return 0;
		int dx = a.i(1) - s->scroll_x, dy = a.i(2) - s->scroll_y;
		s->src.l += dx, s->src.r += dx, s->src.t += dy, s->src.b += dy;
		s->scroll_x = a.i(1), s->scroll_y = a.i(2);
		s->touch();
		return 1;
	});
	def(0x1c2, "sprite_filter", [](Args &a) {
		Sprite *s = Object::as<Sprite>(a.u(0));
		if (!s) return 0;
		s->filter = a.i(1);
		s->touch();
		return 1;
	});
	def(0x1c7, "sprite_set_image", [&e](Args &a) {
		Sprite *s = Object::as<Sprite>(a.u(0));
		ImageRef img = e.image_of(a.u(1));
		if (!s || !img) return 0;
		if (a.i(2) == -1) {
			s->set_image_keep(img);
			return 1;
		}
		Rect r;
		bool has = rect_arg(a, 2, r);
		int x = has ? a.i(6) : 0, y = has ? a.i(7) : 0;
		s->init(img, has ? &r : nullptr, x, y);
		return 1;
	});
}

}  // namespace ore
