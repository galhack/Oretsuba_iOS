// UI natives N_208..N_211, N_2a7..N_2b0, N_2b4..N_2e5 and the list-box
// natives N_2ec..N_2f0 (docs/re/60_ui.md §8). The window natives (N_2b1..N_2b3,
// N_2e6..N_2eb, N_2f1..N_30c) belong to the text system.
#include <functional>
#include <memory>

#include "core/args.h"
#include "core/engine.h"
#include "gfx/surface.h"
#include "text/font.h"
#include "ui/controls.h"
#include "ui/frame_anim.h"
#include "ui/gmp.h"
#include "ui/ui_system.h"
#include "util/log.h"

namespace ore {

namespace {

// Factory settings that are not part of UiSystem (factory +0x38..+0x68).
struct FactoryStyle {
	uint32_t edit_a = 0xff, edit_bg = 0xc0c0dcc0, edit_c = 0;  // N_2e0
	uint32_t list_bg = 0xc0c0dcc0, list_text = 0xffff9cbf, list_sel = 0xc00b2bd7;  // N_2e3
	bool list_marker = true;
};

}  // namespace

void register_ui_natives(Engine &e) {
	auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		e.vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};
	auto gmp = std::make_shared<GmpLoader>();
	auto style = std::make_shared<FactoryStyle>();
	auto ui = [&e]() -> UiSystem & { return *e.ui; };
	auto ctrl = [](Args &a, int k) { return Object::as<UiControl>(a.u(k)); };
	auto handle = [](UiControl *c) { return int32_t(c ? c->handle() : 0); };
	// Natives that take "a layer or an image" (types 4, 7/9/10): its pixels.
	auto image_arg = [&e](uint32_t h) -> ImageRef { return h ? e.image_of(h) : nullptr; };
	// Factory epilogue: own on success, 0 on failure.
	auto made = [ui](std::unique_ptr<UiControl> c, bool ok) -> int32_t {
		if (!ok) return 0;
		return int32_t(ui().own(std::move(c))->handle());
	};

	// ---- GMP layouts and frame animations (§8.1)
	def(0x208, "gmp_load", [ui, gmp](Args &a) {
		return gmp->load(ui(), a.str(0), a.i(1), a.i(2), a.b(3));
	});
	def(0x209, "gmp_free", [gmp](Args &) {
		gmp->clear();
		return 0;
	});
	def(0x20a, "gmp_count", [gmp](Args &) { return int32_t(gmp->entries.size()); });
	def(0x20b, "gmp_entry", [gmp](Args &a) {
		int i = a.i(0);
		if (i < 0 || size_t(i) >= gmp->entries.size()) return 0;
		const GmpLoader::Entry &en = gmp->entries[size_t(i)];
		a.out(1, int32_t(en.handle));
		a.out(2, en.w);
		a.out(3, en.h);
		a.out(4, en.frames);
		return 1;
	});
	def(0x20c, "gmp_entries", [gmp](Args &a) {
		int start = a.i(0) < 0 ? 0 : a.i(0);
		int total = int(gmp->entries.size());
		int n = a.i(1) ? a.i(1) : total;
		if (start + n > total) n = total - start;
		if (n <= 0) return 0;
		Vm &vm = a.vm();
		for (int k = 0; k < n; ++k) {
			const GmpLoader::Entry &en = gmp->entries[size_t(start + k)];
			if (uint32_t p = a.u(2)) vm.mem().wr32(p + uint32_t(k) * 4, en.handle);
			if (uint32_t p = a.u(3)) vm.mem().wr32(p + uint32_t(k) * 4, uint32_t(en.w));
			if (uint32_t p = a.u(4)) vm.mem().wr32(p + uint32_t(k) * 4, uint32_t(en.h));
			if (uint32_t p = a.u(5)) vm.mem().wr32(p + uint32_t(k) * 4, uint32_t(en.frames));
		}
		return 1;
	});
	def(0x20d, "anim_create", [&e, ui, image_arg](Args &a) -> int32_t {
		ImageRef page;
		if (uint32_t h = a.u(4)) {
			page = image_arg(h);
			if (!page) return 1;
		}
		bool keyed = false, keep8 = false;
		uint32_t ck = 0xffffffffu;
		if (a.u(9) != 0xffff) {
			keyed = a.b(9);
			keep8 = a.b(10);
			ck = a.u(11);
		}
		auto anim = std::make_unique<FrameAnim>(e);
		int r = anim->init(a.str(0), a.i(1), a.i(2), a.u(3), page, a.i(5), a.i(6), a.i(7) & 0xff, a.i(8), keyed,
		                   keep8, ck);
		if (r != 0) {
			log_warn("ui: N_20d %s failed (%d)", a.s(0), r);
			return 0;
		}
		FrameAnim *p = anim.get();
		ui().frame_anims.push_back(std::move(anim));
		return int32_t(p->handle());
	});
	def(0x20e, "anim_add_sprite", [](Args &a) {
		if (FrameAnim *f = Object::as<FrameAnim>(a.u(0))) f->add_sprite(a.u(1));
		return 0;
	});
	def(0x20f, "anim_remove_sprite", [](Args &a) {
		if (FrameAnim *f = Object::as<FrameAnim>(a.u(0))) f->remove_sprite(a.u(1));
		return 0;
	});
	def(0x210, "anim_free", [ui](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 1;
		uint32_t h = a.vm().mem().rd32(p);
		auto &v = ui().frame_anims;
		for (auto it = v.begin(); it != v.end(); ++it)
			if ((*it)->handle() == h) {
				v.erase(it);  // playing tasks end by themselves (stale handle)
				a.vm().mem().wr32(p, 0);
				return 0;
			}
		return 1;
	});
	def(0x211, "anim_free_all", [&e, ui](Args &) {
		e.tasks.kill_type(FrameAnimTask::kType);
		e.tasks.kill_type(FrameAnimTask::kType + 1);
		ui().frame_anims.clear();
		return 0;
	});

	// ---- factories (§8.2)
	def(0x2a7, "button_file", [ui, made](Args &a) {
		auto b = std::make_unique<Button>(ui());
		bool ok;
		if (a.u(3) == 0xffff) ok = b->init_file(a.str(2), false, true, a.i(0), a.i(1), 2, 255, -1, -1, 0);
		else
			ok = b->init_file(a.str(2), a.b(3), a.b(4), a.i(0), a.i(1), a.i(5) & 0xff, 255, a.i(6), a.i(7), a.i(8));
		return made(std::move(b), ok);
	});
	def(0x2a8, "button_image", [ui, made, image_arg](Args &a) -> int32_t {
		ImageRef img = image_arg(a.u(2));
		if (!img) return -1;
		auto b = std::make_unique<Button>(ui());
		bool ok;
		if (a.u(3) == 0xffff) ok = b->init_image(img, false, true, a.i(0), a.i(1), 2, 255, 0);
		else ok = b->init_image(img, a.b(3), a.b(4), a.i(0), a.i(1), a.i(5) & 0xff, 255, a.i(6));
		return made(std::move(b), ok);
	});
	def(0x2a9, "check_file", [ui, made](Args &a) {
		auto c = std::make_unique<CheckButton>(ui());
		bool ok;
		if (a.u(4) == 0xffff) {
			ok = c->init_file(a.str(2), a.i(3), false, nullptr, a.i(0), a.i(1), 2, 255, -1, -1, 0);
		} else {
			auto *lead = Object::as<CheckButton>(a.u(5));
			ok = c->init_file(a.str(2), a.i(3), a.b(4), lead, a.i(0), a.i(1), a.i(6) & 0xff, 255, a.i(7), a.i(8),
			                  a.i(9));
		}
		return made(std::move(c), ok);
	});
	def(0x2aa, "check_image", [ui, made, image_arg](Args &a) -> int32_t {
		ImageRef img = image_arg(a.u(2));
		if (!img) return -1;
		auto c = std::make_unique<CheckButton>(ui());
		bool ok;
		if (a.u(4) == 0xffff) {
			ok = c->init_image(img, a.i(3) & 0xff, false, nullptr, a.i(0), a.i(1), 2, 255, 0);
		} else {
			auto *lead = Object::as<CheckButton>(a.u(5));
			ok = c->init_image(img, a.i(3) & 0xff, a.b(4), lead, a.i(0), a.i(1), a.i(6) & 0xff, 255, a.i(7));
		}
		return made(std::move(c), ok);
	});
	def(0x2ab, "static_create", [](Args &) { return 0; });  // type 2, unused
	def(0x2ac, "edit_create_simple", [ui, made, style](Args &a) {
		auto c = std::make_unique<EditBox>(ui());
		bool ok = c->init(Object::as<Font>(a.u(2)), 2, a.i(3), a.i(0), a.i(1), 2, 2, 1, a.i(4), style->edit_bg);
		return made(std::move(c), ok);
	});
	def(0x2ad, "edit_create", [ui, made, style](Args &a) {
		auto c = std::make_unique<EditBox>(ui());
		Font *font = Object::as<Font>(a.u(2));
		bool ok;
		if (a.u(5) == 0xffff) ok = c->init(font, a.i(3), a.i(4), a.i(0), a.i(1), 2, 2, 1, 0, style->edit_bg);
		else ok = c->init(font, a.i(3), a.i(4), a.i(0), a.i(1), a.i(5), a.i(6), a.i(7), a.i(8), style->edit_bg);
		return made(std::move(c), ok);
	});
	def(0x2ae, "scrollbar_drawn", [](Args &) { return 0; });  // procedural bar, unused
	def(0x2af, "scrollbar_create", [ui, made](Args &a) {
		auto c = std::make_unique<ScrollBar>(ui());
		bool ok;
		auto s = [&a](int k) { return a.null(k) ? std::string() : a.str(k); };
		if (a.u(5) == 0xffff) {
			ok = c->init(a.i(0), a.i(1), a.i(2), a.i(3), s(4), 0, 0, "", 0, 0, "", "", false, true, 2, 255, -1, -1,
			             0);
		} else if (a.u(12) == 0xffff) {
			ok = c->init(a.i(0), a.i(1), a.i(2), a.i(3), s(4), a.i(5), a.i(6), s(7), a.i(8), a.i(9), s(10), s(11),
			             false, true, 2, 255, -1, -1, 0);
		} else {
			ok = c->init(a.i(0), a.i(1), a.i(2), a.i(3), s(4), a.i(5), a.i(6), s(7), a.i(8), a.i(9), s(10), s(11),
			             a.b(12), a.b(13), a.i(14) & 0xff, 255, a.i(15), a.i(16), a.i(17));
		}
		return made(std::move(c), ok);
	});
	def(0x2b0, "scrollbar_range", [](Args &a) {
		auto *sb = Object::as<ScrollBar>(a.u(0));
		if (!sb) return -1;
		return sb->set_range(a.i(1), a.i(2), a.i(3), a.i(4), a.i(5), a.i(6));
	});
	// (x, y, w, h, warpX, warpY, b6, b7); p4 == -1 → no warp target. The two
	// flags are not used by anything we know of.
	def(0x2b4, "hit_region", [ui, made](Args &a) {
		auto c = std::make_unique<HitRegion>(ui());
		c->init(a.i(0), a.i(1), a.i(2), a.i(3), true);
		if (a.i(4) == -1) c->set_warp(-1, -1);
		else c->set_warp(a.i(4), a.i(5));
		return made(std::move(c), true);
	});
	def(0x2b5, "listbox_create", [ui, made, style](Args &a) {
		auto c = std::make_unique<ListBox>(ui());
		Font *font = Object::as<Font>(a.u(2));
		std::string knob = a.null(5) ? std::string() : a.str(5);
		bool ok;
		if (a.u(11) == 0xffff) {
			ok = c->init(font, a.i(3), a.i(4), a.i(0), a.i(1), knob, a.i(6), a.i(7), a.i(8), a.i(9), a.i(10), 2, "", 3,
			             0, style->list_bg, style->list_text, style->list_sel, style->list_marker);
		} else {
			std::string marker = a.null(12) ? std::string() : a.str(12);
			ok = c->init(font, a.i(3), a.i(4), a.i(0), a.i(1), knob, a.i(6), a.i(7), a.i(8), a.i(9), a.i(10),
			             a.i(11) & 0xff, marker, a.i(13), a.i(14), style->list_bg, style->list_text, style->list_sel,
			             style->list_marker);
		}
		return made(std::move(c), ok);
	});

	// ---- group / factory state (§8.3)
	def(0x2b6, "ui_parent", [ui](Args &a) {
		Surface *s = Object::as<Surface>(a.u(0));
		if (!s) return 1;
		ui().set_parent(s);
		return 0;
	});
	def(0x2b7, "ui_tooltips", [ui](Args &a) {
		UiSystem &u = ui();
		u.tip_enabled = a.b(0);
		u.tip_delay = a.i(1);
		u.tip_duration = a.i(2);
		u.tip_font = a.u(3);
		if (!u.tip_enabled) u.hide_tooltip();
		return 0;
	});
	def(0x2b8, "ui_cursor", [ui](Args &a) {
		static const int kOffset[7] = {0x14, 0x18, 0x14, 0x16, 9, 4, 0x16};
		int n = a.i(0);
		if (n < 0 || n > 6) return 0;
		ui().cursor_shape = n;
		ui().tip_offset_y = kOffset[n];
		return 1;
	});
	def(0x2b9, "ui_reset", [ui](Args &) {
		ui().reset();
		return 0;
	});
	def(0x2ba, "ui_update", [&e, ui](Args &) { return ui().update(e.now); });
	def(0x2bb, "ui_event", [ui](Args &) { return ui().take_event(); });
	def(0x2bc, "ui_set_focus", [ui, ctrl](Args &a) {
		ui().set_focus(ctrl(a, 0));
		return 0;
	});
	def(0x2bd, "ui_focus", [ui, handle](Args &) { return handle(ui().focus()); });
	def(0x2be, "ui_hover", [ui, handle](Args &) { return handle(ui().hover()); });
	def(0x2bf, "ui_destroy_all", [ui](Args &) {
		ui().destroy_all();
		return 0;
	});
	def(0x2c0, "ui_parent_b", [ui](Args &a) {
		int old = ui().parent_b ? 1 : 0;
		ui().parent_b = a.b(0);
		return old;
	});
	def(0x2c1, "ui_tag", [ui](Args &a) {
		ui().creation_tag = a.i(0);
		return 0;
	});
	def(0x2c2, "ui_destroy_tag", [ui](Args &a) {
		ui().destroy_by_tag(a.i(0), a.b(1));
		return 0;
	});
	def(0x2c3, "ui_destroy", [ui](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 1;
		UiControl *c = Object::as<UiControl>(a.vm().mem().rd32(p));
		if (!c || !ui().destroy(c)) return 1;
		a.vm().mem().wr32(p, 0);
		return 0;
	});
	def(0x2c4, "ui_add", [ui, ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		if (!c) return -1;
		return ui().add_to_group(c, a.i(1));
	});
	def(0x2c5, "ui_remove", [ui, ctrl](Args &a) {
		ui().remove_from_group(ctrl(a, 0));
		return 0;
	});
	def(0x2c6, "ui_clear", [ui](Args &) {
		ui().clear_group();
		return 0;
	});
	def(0x2c7, "ui_text_input", [](Args &) { return 0; });  // IME subclass: platform text entry instead
	def(0x2c8, "ui_ime_helper", [](Args &) { return 0; });
	def(0x2c9, "ui_push", [ui](Args &) { return ui().push_group(); });
	def(0x2ca, "ui_pop", [ui](Args &) { return ui().pop_group(); });

	// ---- generic control calls (§8.4)
	def(0x2cb, "ctrl_set_id", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		if (!c) return 0;
		c->id = a.i(1);
		return 1;
	});
	def(0x2cc, "ctrl_tooltip", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		if (!c) return 0;
		c->tooltip = a.str(1);
		return 1;
	});
	def(0x2cd, "ctrl_neighbours", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		if (!c) return 0;
		c->neighbour[2] = ctrl(a, 1);  // left
		c->neighbour[3] = ctrl(a, 2);  // right
		c->neighbour[0] = ctrl(a, 3);  // up
		c->neighbour[1] = ctrl(a, 4);  // down
		return 1;
	});
	def(0x2ce, "ctrl_sounds", [](Args &) { return 0; });  // unused
	def(0x2cf, "ctrl_enable", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		return c && c->set_enabled(a.b(1)) ? 1 : 0;
	});
	def(0x2d0, "ctrl_enabled", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		return c && c->enabled() ? 1 : 0;
	});
	def(0x2d1, "ctrl_show", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		if (!c) return 0;
		c->set_visible(a.b(1));
		return 1;
	});
	def(0x2d2, "ctrl_visible", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		return c && c->visible() ? 1 : 0;
	});
	def(0x2d3, "ctrl_move", [ctrl](Args &a) {
		if (UiControl *c = ctrl(a, 0)) c->set_pos(a.i(1), a.i(2));
		return 0;
	});
	def(0x2d4, "ctrl_z", [ctrl](Args &a) {
		if (UiControl *c = ctrl(a, 0)) c->set_z(a.i(1));
		return 0;
	});
	def(0x2d5, "ctrl_width", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		return c ? c->width() : 0;
	});
	def(0x2d6, "ctrl_height", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		return c ? c->height() : 0;
	});
	def(0x2d7, "ctrl_sprite", [ctrl](Args &a) -> int32_t {
		UiControl *c = ctrl(a, 0);
		if (!c) return -1;
		Layer *l = c->sprite(a.i(1));
		return l ? int32_t(l->handle()) : 0;
	});
	def(0x2d8, "ctrl_image", [ui, ctrl](Args &a) -> int32_t {
		UiControl *c = ctrl(a, 0);
		if (!c) return -1;
		ImageRef img = c->image(a.i(1));
		return img ? int32_t(ui().image_handle(img)) : 0;
	});
	def(0x2d9, "ctrl_set_value", [ctrl](Args &a) -> int32_t {
		UiControl *c = ctrl(a, 0);
		if (!c) return -1;
		int t = c->ui_type();
		if (t == 3 || t == 5) c->set_value_str(a.null(1) ? nullptr : a.s(1));
		else c->set_value_int(a.i(1));
		return 0;
	});
	def(0x2da, "ctrl_value", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		return c ? c->value() : -1;
	});
	def(0x2db, "ctrl_rect_hit", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		if (!c) return -1;
		int t = c->ui_type();
		if (t != 0 && t != 1 && t != 4) return -1;
		c->set_rect_hit(a.b(1));
		return 0;
	});
	def(0x2dc, "ctrl_active_mode", [ctrl](Args &a) {
		UiControl *c = ctrl(a, 0);
		if (!c) return -1;
		int t = c->ui_type();
		if (t != 1 && t != 3 && t != 5 && t != 0x6f && t != 6) return -1;
		c->set_active_mode(a.b(1));
		return 0;
	});
	def(0x2dd, "factory_colors", [](Args &) { return 0; });  // unused
	def(0x2de, "check_select", [](Args &a) {
		auto *c = Object::as<CheckButton>(a.u(0));
		if (!c) return -1;
		c->select_index(a.i(1));
		return 0;
	});
	def(0x2df, "check_selected", [](Args &a) {
		auto *c = Object::as<CheckButton>(a.u(0));
		return c ? c->selected_index() : -1;
	});
	def(0x2e0, "edit_style", [style](Args &a) {
		style->edit_a = a.u(0);
		style->edit_bg = a.u(1);
		style->edit_c = a.u(2);
		return 0;
	});
	def(0x2e1, "edit_style2", [](Args &) { return 0; });  // unused
	def(0x2e2, "edit_style3", [](Args &) { return 0; });  // unused
	def(0x2e3, "list_style", [style](Args &a) {
		style->list_bg = a.u(0);
		style->list_text = a.u(1);
		style->list_sel = a.u(2);
		style->list_marker = a.b(3);
		return 0;
	});
	def(0x2e4, "scrollbar_arrows", [handle](Args &a) {
		auto *sb = Object::as<ScrollBar>(a.u(0));
		if (!sb) return 0;
		a.out(1, handle(sb->arrow(0)));
		a.out(2, handle(sb->arrow(1)));
		return 1;
	});
	def(0x2e5, "region_warp", [](Args &a) {
		auto *r = Object::as<HitRegion>(a.u(0));
		if (!r) return 0;
		r->set_warp(a.i(1), a.i(2));
		return 1;
	});

	// ---- list box (type 9 only; §8.5)
	def(0x2ec, "list_add", [](Args &a) {
		auto *lb = Object::as<ListBox>(a.u(0));
		if (!lb) return -1;
		return lb->add_item(a.null(1) ? nullptr : a.s(1), a.null(2) ? nullptr : a.s(2), a.b(3));
	});
	def(0x2ed, "list_layout", [](Args &a) {
		auto *lb = Object::as<ListBox>(a.u(0));
		if (!lb) return -1;
		if (a.b(1)) lb->relayout();
		else lb->redraw();
		return 0;
	});
	def(0x2ee, "list_set_marker", [](Args &a) {
		auto *lb = Object::as<ListBox>(a.u(0));
		if (!lb) return -1;
		return lb->set_marker(a.i(1), a.i(2), a.b(3));
	});
	def(0x2ef, "list_marker", [](Args &a) {
		auto *lb = Object::as<ListBox>(a.u(0));
		return lb ? lb->marker(a.i(1)) : 0;
	});
	def(0x2f0, "list_hover", [](Args &a) {
		auto *lb = Object::as<ListBox>(a.u(0));
		return lb ? lb->hover_index() : -1;
	});
}

}  // namespace ore
