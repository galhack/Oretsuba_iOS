// Text natives: fonts N_a3..N_bf, N_c, N_e8, message manager N_1cc..N_1f7,
// LAG/LAD objects N_1f8..N_202, lip-sync N_214/N_215 and the ADV window
// natives N_2b1..N_2b3, N_2e6..N_30c (docs/re/40_text.md §8,
// docs/re/60_ui.md §8.5, docs/re/10_lifecycle_system.md §7.6).
#include <algorithm>
#include <cstring>

#include "core/args.h"
#include "core/engine.h"
#include "gfx/surface.h"
#include "text/font.h"
#include "text/text_impl.h"
#include "text/windows.h"
#include "ui/frame_anim.h"
#include "util/log.h"

namespace ore {

Layer *layer_of(uint32_t h);  // gfx/natives_gfx.cpp

namespace {

Font *font_of(uint32_t h) {
	return Object::as<Font>(h);
}

uint32_t rgb3(Args &a, int k) {
	return (a.u(k) & 0xff) << 16 | (a.u(k + 1) & 0xff) << 8 | (a.u(k + 2) & 0xff);
}

// Pixels of a graphic object (image, page or sprite) for the text draw natives.
ImageRef target_image(Engine &e, uint32_t h) {
	if (ImageRef i = e.image_of(h)) return i;
	if (Sprite *s = dynamic_cast<Sprite *>(layer_of(h))) return s->image;
	return nullptr;
}

// Minimal wsprintf over 32-bit argument words (%d %u %x %X %s %c, width/zero flag).
std::string mini_format(Args &a, const char *fmt, int first) {
	std::string out;
	int k = first;
	for (const char *p = fmt; *p; ++p) {
		if (*p != '%') {
			out += *p;
			continue;
		}
		++p;
		if (*p == '%') {
			out += '%';
			continue;
		}
		std::string spec = "%";
		while (*p && strchr("-+0 #123456789", *p)) spec += *p++;
		if (!*p) break;
		char conv = *p;
		char buf[64];
		if (conv == 's') {
			out += a.s(k++);
			continue;
		}
		if (!strchr("duxXc", conv)) continue;
		spec += conv;
		snprintf(buf, sizeof buf, spec.c_str(), a.i(k++));
		out += buf;
	}
	return out;
}

const char *kFontsSjis[] = {"\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", "\x82\x6c\x82\x72 \x96\xbe\x92\xa9"};
const char *kFontsGbk[] = {"\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", "\xba\xda\xcc\xe5", "\xcb\xce\xcc\xe5"};

}  // namespace

void register_text_natives(Engine &e) {
	auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		e.vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};
	auto mm = [&e]() -> MessageManager & { return e.text->msg(); };
	auto ts = [&e]() -> TextSystem::Impl & { return *e.text->impl; };

	// ---------------------------------------------------------------- fonts
	auto create_font = [&e](Args &a, bool rich) -> int32_t {
		auto f = std::make_unique<Font>(e, std::max(1, a.i(1)), a.s(0), rich);
		if (!rich) {
			f->edge_radius = 1;
			f->edge_density = 0x1000;
		}
		uint32_t h = f->handle();
		e.fonts.push_back(std::move(f));
		return int32_t(h);
	};
	def(0xa3, "font_new_simple", [create_font](Args &a) { return create_font(a, false); });
	def(0xa4, "font_new", [create_font](Args &a) { return create_font(a, true); });
	def(0xa5, "font_delete", [&e](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 0;
		uint32_t h = e.vm.mem().rd32(p);
		for (auto it = e.fonts.begin(); it != e.fonts.end(); ++it)
			if ((*it)->handle() == h) {
				if (e.default_font == it->get()) e.default_font = nullptr;
				e.fonts.erase(it);
				e.vm.mem().wr32(p, 0);
				break;
			}
		return 0;
	});
	def(0xa6, "font_set_default", [&e](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return 0;
		e.default_font = f;
		return 1;
	});
	def(0xa7, "font_face", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return -1;
		f->set_face(a.s(1));
		return 0;
	});
	def(0xa8, "font_size", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return -1;
		f->set_size(a.i(1));
		return 0;
	});
	def(0xa9, "font_style", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return -1;
		f->set_style(uint8_t(a.u(1)) != 0, uint8_t(a.u(2)) != 0);
		return 0;
	});
	def(0xaa, "font_rotation", [](Args &a) { return font_of(a.u(0)) ? 0 : -1; });
	def(0xab, "font_antialias", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return 0;
		f->set_antialias(int8_t(a.u(1)));
		return 1;
	});
	def(0xac, "font_color", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return 0;
		f->color = rgb3(a, 1);
		return 1;
	});
	def(0xad, "font_effect", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return 0;
		f->effect = uint8_t(a.u(1));
		f->shadow_dx = a.i(2);
		f->shadow_dy = a.i(3);
		return 1;
	});
	def(0xae, "font_effect_color", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return 0;
		f->effect_color = rgb3(a, 1);
		return 1;
	});
	def(0xaf, "font_misc", [](Args &a) { return font_of(a.u(0)) ? 1 : 0; });
	def(0xb0, "font_line_spacing", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return 0;
		f->line_spacing = a.i(1);
		return 1;
	});
	def(0xb1, "font_simple_opt1", [](Args &a) {
		Font *f = font_of(a.u(0));
		return f ? (f->rich ? -2 : 0) : -1;
	});
	def(0xb2, "font_simple_opt2", [](Args &a) {
		Font *f = font_of(a.u(0));
		return f ? (f->rich ? -2 : 0) : -1;
	});
	def(0xb3, "font_rich_opt", [](Args &a) {
		Font *f = font_of(a.u(0));
		return f && f->rich ? 1 : 0;
	});
	def(0xb4, "font_rich_opt2", [](Args &a) {
		Font *f = font_of(a.u(0));
		return f ? (f->rich ? 0 : -2) : -1;
	});
	def(0xb5, "font_gradient", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return -1;
		if (!f->rich) return -2;
		f->gradient = uint8_t(a.u(1)) != 0;
		f->grad_top = a.u(2) & 0xffffff;
		f->grad_bottom = a.u(3) & 0xffffff;
		return 0;
	});
	def(0xb6, "font_edge", [](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f || !f->rich) return 0;
		if (a.i(1) > 0) f->edge_density = a.i(1);
		if (a.i(2) > 0) f->edge_radius = std::min(a.i(2), 8);
		f->invalidate_cache();
		return 1;
	});
	auto measure = [&e](Args &a) {
		Font *f = font_of(a.u(0));
		if (!f) return -1;
		int w = 0, h = 0, ox = 0, oy = 0;
		int n = f->measure(a.s(1), w, h, &ox, &oy);
		a.out(2, w);
		a.out(3, h);
		a.out(4, ox);
		a.out(5, oy);
		(void)e;
		return n;
	};
	def(0xb7, "font_measure", measure);
	def(0xb8, "font_measure2", measure);
	auto draw_on = [&e](Args &a, const std::string &text, uint32_t fh) -> int32_t {
		ImageRef img = target_image(e, a.u(0));
		if (!img) return -1;
		Font *f = fh ? font_of(fh) : e.default_font;
		if (!f) return 2;
		f->draw(*img, a.i(1), a.i(2), text, true);
		gfx_touch();
		return 0;
	};
	def(0xb9, "draw_text_fmt", [draw_on](Args &a) { return draw_on(a, mini_format(a, a.s(3), 4), 0); });
	def(0xba, "draw_text", [draw_on](Args &a) { return draw_on(a, a.s(3), a.u(4)); });
	def(0xbb, "draw_text2", [draw_on](Args &a) { return draw_on(a, a.s(3), a.u(4)); });
	def(0xbc, "draw_text3", [draw_on](Args &a) { return draw_on(a, a.s(3), a.u(4)); });
	def(0xbd, "font_enum", [&e, ts](Args &) {
		auto &l = ts().font_list;
		l.clear();
		if (e.encoding == TextEncoding::Gbk)
			for (const char *s : kFontsGbk) l.push_back(s);
		else
			for (const char *s : kFontsSjis) l.push_back(s);
		return int32_t(l.size());
	});
	def(0xbe, "font_enum_copy", [ts](Args &a) {
		auto &l = ts().font_list;
		if (a.u(0) >= l.size()) {
			a.out_str(1, std::string());
			return 0;
		}
		a.out_str(1, l[a.u(0)]);
		return 1;
	});
	def(0xbf, "font_enum_name", [&e, ts](Args &a) -> int32_t {
		auto &t = ts();
		if (a.u(0) >= t.font_list.size()) return 0;
		if (t.font_list_mem.size() < t.font_list.size()) t.font_list_mem.resize(t.font_list.size(), 0);
		uint32_t &m = t.font_list_mem[a.u(0)];
		if (!m) m = e.vm.mem().alloc_string(t.font_list[a.u(0)], "fontname");
		return int32_t(m);
	});

	// N_c: re-apply message-window settings from the option block (FUN_0041d770)
	def(0xc, "apply_text_options", [&e, mm](Args &a) {
		bool dirty = e.text->options_dirty();
		if (!dirty && uint8_t(a.u(0))) return 0;
		int mode = e.opt8(0xe85), alpha = e.opt8(0xe86);
		uint32_t rgb = e.opt32(0xe88);
		if (TextWindow *w = Object::as<TextWindow>(a.u(2))) w->set_bg_style(mode, rgb, alpha);
		if (SelectWindow *s = Object::as<SelectWindow>(a.u(3))) {
			s->set_bg_style(mode, rgb, alpha);
			s->mark_chosen = e.opt8(0xe44) != 0;
		}
		if (uint8_t(a.u(5))) {
			Font *f = a.u(4) ? font_of(a.u(4)) : e.default_font;
			if (f && f->rich) {
				const char *fp = e.vm.mem().cstr(e.opt_addr(0xe60));
				std::string face(fp, strnlen(fp, 32));
				f->effect = e.opt8(0xe82);
				f->shadow_dx = f->shadow_dy = 2;
				f->gradient = e.opt8(0xe81) != 0;
				f->set_antialias(e.opt8(0xe80));
				bool bold = e.opt8(0xe83) != 0;
				if (bold != f->bold || face != f->face) {
					if (!face.empty()) f->set_face(face);
					f->set_style(bold, f->italic);
				}
			}
		}
		if (uint8_t(a.u(6))) {
			int n = e.opt8(0xe5a);
			int cap = n ? n + 1 : 0;
			if (cap != mm().capacity) mm().set_capacity(cap);
		}
		if (uint8_t(a.u(1))) e.text->clear_options_dirty();
		return dirty ? 1 : 0;
	});

	// N_e8: text rendered into a new image object (FUN_0049dd40)
	def(0xe8, "text_image", [&e](Args &a) -> int32_t {
		std::string text = a.s(0);
		if (text.empty()) return 0;
		Font *f = a.u(1) ? font_of(a.u(1)) : e.default_font;
		if (!f) return 0;
		int w = 0, h = 0, ox = 0, oy = 0;
		f->measure(text, w, h, &ox, &oy);
		auto img = std::make_shared<Image>(std::max(w, 1), std::max(h, 1), 32);
		img->has_alpha = true;
		f->draw(*img, -ox, -oy, text, true);
		img->tag = e.image_tag;
		return int32_t(e.own_image(std::make_unique<ImageObj>(img))->handle());
	});

	// ---------------------------------------------------------------- message manager
	def(0x1cc, "log_capacity", [mm](Args &a) {
		mm().set_capacity(uint8_t(a.u(0)));
		return 0;
	});
	def(0x1cd, "log_enable", [mm](Args &a) {
		mm().log_enabled = uint8_t(a.u(0)) != 0;
		return 0;
	});
	def(0x1ce, "set_message", [mm](Args &a) { return mm().set_message(a.u(0)); });
	def(0x1cf, "message_enable", [mm](Args &a) {
		mm().enabled = uint8_t(a.u(0)) != 0;
		return 0;
	});
	for (int n : {0x1d0, 0x1d2, 0x1d3, 0x1d4, 0x1d6, 0x1f2})
		def(n, "msg_render_unused", [](Args &) { return 0; });
	def(0x1d1, "name_segment", [mm](Args &a) {
		if (!mm().has_name_segment(a.i(1))) return 0;
		std::string s = mm().name_segment(a.i(1));
		if (s.empty() && a.i(1) > 0) return 0;
		a.out_str(0, s);
		return int32_t(s.size() + 1);
	});
	def(0x1d5, "msg_skip_tags", [mm](Args &) {
		mm().rewind();
		return 0;
	});
	def(0x1d7, "msg_cursor", [](Args &a) {
		a.out(0, 0);
		a.out(1, 0);
		return 0;
	});
	def(0x1d8, "msg_last_string", [mm](Args &a) {
		a.out_str(0, mm().last_string);
		return mm().last_string.empty() ? 0 : 1;
	});
	def(0x1d9, "msg_last_size", [mm](Args &) { return mm().last_size; });
	def(0x1da, "log_can_prev", [mm](Args &) { return mm().view > 0 ? 1 : 0; });
	def(0x1db, "log_can_next", [mm](Args &) { return mm().view < mm().used - 1 ? 1 : 0; });
	def(0x1dc, "msg_plain_text", [mm](Args &a) {
		std::string s = mm().plain_text(true);
		if (!a.null(0)) a.out_str(0, s);
		return int32_t(s.size() + 1);
	});
	def(0x1dd, "log_count", [mm](Args &) { return mm().used; });
	def(0x1de, "log_cursor", [mm](Args &) { return mm().view; });
	def(0x1df, "log_prev", [mm](Args &) { return mm().load_backlog(mm().view - 1); });
	def(0x1e0, "log_next", [mm](Args &) { return mm().load_backlog(mm().view + 1); });
	def(0x1e1, "log_oldest", [mm](Args &) { return mm().load_backlog(0); });
	def(0x1e2, "log_newest", [mm](Args &) { return mm().load_backlog(mm().used - 1); });
	def(0x1e3, "log_load", [mm](Args &a) {
		int k = std::min(std::max(a.i(0), 0), mm().used - 1);
		return mm().load_backlog(k);
	});
	def(0x1e4, "log_move", [mm](Args &a) { return mm().load_backlog(mm().view + a.i(0)); });
	def(0x1e5, "msg_bookmark", [mm](Args &) {
		mm().bookmark = mm().pos;
		return 1;
	});
	def(0x1e6, "msg_restore", [mm](Args &) {
		mm().pos = mm().bookmark;
		return 1;
	});
	def(0x1e7, "msg_clear", [&e](Args &) {
		e.text->clear_message_state();
		return 0;
	});
	def(0x1e8, "msg_read", [mm](Args &) { return mm().read ? 1 : 0; });
	def(0x1eb, "set_choices", [mm](Args &a) { return mm().set_choices(a.u(0)); });
	def(0x1ec, "choices_enable", [mm](Args &a) {
		mm().choices_enabled = uint8_t(a.u(0)) != 0;
		return 0;
	});
	def(0x1ed, "choice_capacity", [mm](Args &a) {
		mm().clear_choices();
		mm().choice_capacity = std::max(0, a.i(0));
		return 0;
	});
	def(0x1ee, "choice_reset_base", [mm](Args &) {
		mm().choice_base = 0;
		mm().choice_bits = 0;
		return 0;
	});
	def(0x1ef, "choice_add", [mm](Args &a) {
		MessageManager &m = mm();
		if (int(m.choices.size()) >= m.choice_capacity) return -1;
		std::string s = a.s(0);
		if (s.size() > 253) s.resize(253);
		m.choices.push_back(s);
		m.shown_since_choices = false;
		return 0;
	});
	def(0x1f0, "choice_clear", [mm](Args &) {
		mm().clear_choices();
		return 0;
	});
	def(0x1f1, "choice_count", [mm](Args &) { return int32_t(mm().choices.size()); });
	def(0x1f3, "choice_copy", [mm](Args &a) {
		MessageManager &m = mm();
		if (a.i(0) < 0 || a.i(0) >= int(m.choices.size())) return 1;
		if (a.null(1)) return 2;
		const std::string &s = m.choices[size_t(a.i(0))];
		a.out_str(1, s, size_t(std::max(a.i(2), 1)));
		return s.size() + 1 > size_t(std::max(a.i(2), 0)) ? 3 : 0;
	});
	def(0x1f4, "choice_string", [mm](Args &a) { return int32_t(mm().choice_ptr(a.i(0))); });
	def(0x1f5, "choice_select", [mm](Args &a) { return mm().select_choice(uint8_t(a.u(0))); });
	def(0x1f6, "choice_selected", [mm](Args &) { return int32_t(mm().selected); });
	def(0x1f7, "choice_chosen", [mm](Args &a) { return mm().choice_chosen(uint8_t(a.u(0))) ? 1 : 0; });

	// ---------------------------------------------------------------- LAG / LAD objects
	def(0x1f8, "lag_load", [&e, ts](Args &a) -> int32_t {
		auto d = std::make_unique<LagData>();
		if (!d->load(e, a.s(0), a.b(1))) return 0;
		uint32_t h = d->handle();
		ts().lags.push_back(std::move(d));
		return int32_t(h);
	});
	def(0x1f9, "lag_free", [&e, ts](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 0;
		uint32_t h = e.vm.mem().rd32(p);
		auto &l = ts().lags;
		for (auto it = l.begin(); it != l.end(); ++it)
			if ((*it)->handle() == h) {
				l.erase(it);
				e.vm.mem().wr32(p, 0);
				return 0;
			}
		return 1;
	});
	def(0x1fa, "lag_free_all", [&e, ts](Args &) {
		e.tasks.kill_type(LagPlayer::kType);
		ts().lags.clear();
		return 0;
	});
	def(0x1fb, "lag_reload", [&e](Args &a) {
		LagData *d = Object::as<LagData>(a.u(0));
		if (!d) return 1;
		return d->load(e, a.s(1), a.b(2)) ? 0 : 2;
	});
	def(0x1fc, "lag_play", [&e](Args &a) -> int32_t {
		Surface *s = e.surface_of(a.u(0));
		LagData *d = Object::as<LagData>(a.u(1));
		if (!s || !s->comp() || !d) return 0;
		bool catch_up = true, remove = true;
		if (a.i(4) != -1) {
			catch_up = a.b(5);
			remove = a.b(6);
		}
		auto t = std::make_unique<LagPlayer>();
		if (t->start(d, &s->comp()->main, a.i(2), a.i(3), catch_up, remove)) return 0;
		uint32_t h = t->handle();
		e.tasks.add(std::move(t));
		return int32_t(h);
	});
	def(0x1fd, "lag_state", [&e](Args &a) {
		LagPlayer *t = Object::as<LagPlayer>(a.u(0));
		return t && e.tasks.contains(t) && !t->killed ? t->state : 0;
	});
	auto load_anim = [&e](AnimData &d, const std::string &path) {
		if (!e.read_file(path, d.data) || d.data.size() < 20) return false;
		uint32_t magic;
		memcpy(&magic, d.data.data(), 4);
		if (magic != 0x44414c) return false;
		d.path = path;
		memcpy(&d.field_8, &d.data[4], 2);
		memcpy(&d.field_a, &d.data[6], 2);
		memcpy(&d.field_e, &d.data[10], 2);
		return true;
	};
	def(0x1fe, "anim_load", [ts, load_anim](Args &a) -> int32_t {
		auto d = std::make_unique<AnimData>();
		if (!load_anim(*d, a.s(0))) return 0;
		uint32_t h = d->handle();
		ts().anims.push_back(std::move(d));
		return int32_t(h);
	});
	def(0x1ff, "anim_free", [&e, ts](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 0;
		uint32_t h = e.vm.mem().rd32(p);
		auto &l = ts().anims;
		for (auto it = l.begin(); it != l.end(); ++it)
			if ((*it)->handle() == h) {
				l.erase(it);
				e.vm.mem().wr32(p, 0);
				break;
			}
		return 0;
	});
	def(0x200, "anim_free_all", [ts](Args &) {
		ts().anims.clear();
		return 0;
	});
	def(0x201, "anim_reload", [load_anim](Args &a) {
		AnimData *d = Object::as<AnimData>(a.u(0));
		return d && load_anim(*d, a.s(1)) ? 0 : 1;
	});
	def(0x202, "anim_field", [](Args &a) -> int32_t {
		AnimData *d = Object::as<AnimData>(a.u(0));
		if (!d) return 0;
		switch (a.i(1)) {
		case 0: return d->field_e;
		case 2: return d->field_8;
		case 3: return d->field_a;
		default: return 0;
		}
	});

	// ---------------------------------------------------------------- lip sync
	def(0x214, "lip_sync", [&e](Args &a) -> int32_t {
		uint32_t snd = a.u(1);
		if ((snd & 0xFFFF0000u) != 0x5FFE0000u) return 0;
		int ch = int(snd & 0xFFFF);
		if (!e.sound.channel_playing(ch)) return 0;
		FrameAnim *anim = Object::as<FrameAnim>(a.u(0));
		if (!anim) return 0;
		anim->save_background();
		auto t = std::make_unique<LipSyncTask>(e, a.u(0), ch);
		uint32_t h = t->handle();
		e.tasks.add(std::move(t));
		return int32_t(h);
	});
	def(0x215, "anim_layer", [](Args &a) -> int32_t {
		Layer *l = layer_of(a.u(0));
		return l ? int32_t(l->handle()) : a.i(0);
	});

	// ---------------------------------------------------------------- window factories
	auto make_window = [&e](Args &a, bool msg) -> int32_t {
		if (!e.ui) return 0;
		bool images = a.i(5) != -1;
		const char *b = images && !a.null(6) ? a.s(6) : nullptr;
		const char *al = images && !a.null(7) ? a.s(7) : nullptr;
		const char *g = images && !a.null(8) ? a.s(8) : nullptr;
		uint32_t ruby = images ? a.u(5) : 0;
		std::unique_ptr<TextWindow> w;
		if (msg)
			w = std::make_unique<MessageWindow>(e, *e.ui, e.text->msg());
		else
			w = std::make_unique<TextWindow>(e, *e.ui);
		if (w->create(a.i(0), a.i(1), a.i(2), a.i(3), a.u(4), ruby, b, al, g, a.i(9))) return 0;
		return int32_t(e.ui->own(std::move(w))->handle());
	};
	def(0x2b1, "window_new", [make_window](Args &a) { return make_window(a, false); });
	def(0x2b2, "message_window_new", [make_window](Args &a) { return make_window(a, true); });
	def(0x2b3, "select_window_new", [&e](Args &a) -> int32_t {
		if (!e.ui) return 0;
		auto w = std::make_unique<SelectWindow>(e, *e.ui);
		if (w->create(a.i(0), a.i(1), a.i(2), a.i(3), a.i(4), a.u(5), a.i(6))) return 0;
		return int32_t(e.ui->own(std::move(w))->handle());
	});

	// ---------------------------------------------------------------- window natives
	def(0x2e6, "window_margins", [](Args &a) {
		if (TextWindow *w = Object::as<TextWindow>(a.u(0))) {
			w->set_margins(a.i(1), a.i(2), a.i(3), a.i(4), true);
			w->set_pads(a.i(5), a.i(6), a.i(7), a.i(8), true);
			return 1;
		}
		if (SelectWindow *s = Object::as<SelectWindow>(a.u(0))) {
			s->ml = a.i(1), s->mt = a.i(2), s->mr = a.i(3), s->mb = a.i(4);
			s->tx = a.i(5), s->ty = a.i(6);
			return 1;
		}
		return 0;
	});
	def(0x2e7, "window_opt_unused", [](Args &) { return 0; });
	def(0x2e8, "window_bg_style", [](Args &a) {
		if (TextWindow *w = Object::as<TextWindow>(a.u(0))) {
			w->set_bg_style(uint8_t(a.u(1)), a.u(2), uint8_t(a.u(3)));
			return 0;
		}
		if (SelectWindow *s = Object::as<SelectWindow>(a.u(0))) {
			s->set_bg_style(uint8_t(a.u(1)), a.u(2), uint8_t(a.u(3)));
			return 0;
		}
		return -1;
	});
	def(0x2e9, "window_flag_unused", [](Args &) { return 0; });
	def(0x2ea, "window_clear", [](Args &a) {
		if (TextWindow *w = Object::as<TextWindow>(a.u(0))) {
			w->clear_page();
			return 0;
		}
		if (SelectWindow *s = Object::as<SelectWindow>(a.u(0))) {
			s->clear();
			return 0;
		}
		return -1;
	});
	def(0x2eb, "window_item_pos", [](Args &a) {
		int x = 0, y = 0;
		if (SelectWindow *s = Object::as<SelectWindow>(a.u(0))) {
			bool ok = s->item_pos(a.i(3), x, y);
			if (ok) {
				a.out(1, x);
				a.out(2, y);
			}
			return ok ? 1 : 0;
		}
		if (TextWindow *w = Object::as<TextWindow>(a.u(0))) {
			a.out(1, w->x() + w->cx);
			a.out(2, w->y() + w->cy);
			return 1;
		}
		return 0;
	});
	def(0x2f1, "window_text_options", [](Args &a) {
		TextWindow *w = Object::as<TextWindow>(a.u(0));
		if (!w) return 0;
		w->fullwidth = a.u(1) != 0;
		w->char_spacing = a.i(2);
		w->line_spacing = a.i(3);
		w->autowrap = a.u(4) != 0;
		w->wrap_reserve = a.i(5);
		w->update_wrap();
		w->ruby_gap = a.i(6);
		w->relayout();
		return 1;
	});
	def(0x2f2, "window_min_line", [](Args &a) {
		TextWindow *w = Object::as<TextWindow>(a.u(0));
		if (!w) return 0;
		w->min_line_h = a.i(1);
		w->relayout();
		return 1;
	});
	for (int n = 0x2f3; n <= 0x2f7; ++n) def(n, "window_unused", [](Args &) { return 0; });
	def(0x2f8, "msgwin_click_completes", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		if (!w) return 1;
		w->click_completes = uint8_t(a.u(1)) != 0;
		return 0;
	});
	def(0x2f9, "msgwin_check", [](Args &a) { return Object::as<MessageWindow>(a.u(0)) ? 0 : 1; });
	def(0x2fa, "msgwin_show", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		return w ? w->show(uint8_t(a.u(1)) != 0) : 1;
	});
	def(0x2fb, "msgwin_show_instant", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		return w ? w->show_instant() : 1;
	});
	def(0x2fc, "msgwin_control", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		return w ? w->control(uint8_t(a.u(1))) : 1;
	});
	def(0x2fd, "msgwin_cursor", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		if (!w) return 1;
		w->set_cursor(a.u(1));
		return 0;
	});
	def(0x2fe, "msgwin_name_box", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		if (!w) return 1;
		TextWindow *n = Object::as<TextWindow>(a.u(1));
		if (!n || n->ui_type() != 5) return 2;
		w->name_box = a.u(1);
		w->has_name = false;
		w->name_mode = uint8_t(a.u(2));
		return 0;
	});
	def(0x2ff, "msgwin_skip_button", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		if (!w) return 1;
		UiControl *c = Object::as<UiControl>(a.u(1));
		if (!c || c->ui_type() != 1) return 2;
		w->skip_button = a.u(1);
		return 0;
	});
	def(0x300, "msgwin_state", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		return w ? w->state() : 0;
	});
	def(0x301, "msgwin_skip_flag", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		return w ? w->skip_flag() : 0;
	});
	def(0x302, "msgwin_voice", [](Args &a) {
		MessageWindow *w = Object::as<MessageWindow>(a.u(0));
		if (!w || a.null(1)) return 0;
		a.out_str(1, w->voice_id);
		return w->voice_id.empty() ? 0 : 1;
	});
	def(0x303, "select_unused", [](Args &) { return 0; });
	def(0x304, "select_images", [](Args &a) {
		SelectWindow *s = Object::as<SelectWindow>(a.u(0));
		if (!s) return 4;
		auto str = [&a](int k) -> const char * { return a.null(k) ? nullptr : a.s(k); };
		bool caps = a.i(7) != -1;
		return s->set_images(a.i(1), uint8_t(a.u(2)) != 0, uint8_t(a.u(3)) != 0, str(4), str(5), str(6),
		                     caps ? str(7) : nullptr, caps ? str(8) : nullptr, caps ? str(9) : nullptr,
		                     caps ? str(10) : nullptr, caps ? str(11) : nullptr, caps ? str(12) : nullptr);
	});
	def(0x305, "select_highlight_style", [](Args &a) {
		SelectWindow *s = Object::as<SelectWindow>(a.u(0));
		if (!s) return 0;
		s->hl_mode = uint8_t(a.u(1));
		s->hl_color = a.u(2);
		return 1;
	});
	def(0x306, "select_colors", [](Args &a) {
		SelectWindow *s = Object::as<SelectWindow>(a.u(0));
		if (!s) return 0;
		s->col_normal = a.u(1) & 0xffffff;
		s->col_chosen = a.u(2) & 0xffffff;
		s->col_hover = a.u(3) & 0xffffff;
		s->col_disabled = 0x909090;
		return 1;
	});
	def(0x307, "select_gradients", [](Args &a) {
		SelectWindow *s = Object::as<SelectWindow>(a.u(0));
		if (!s) return 0;
		for (int k = 0; k < 6; ++k) s->grad[k] = a.u(1 + k) & 0xffffff;
		s->grad[6] = 0xb0b0b0;
		s->grad[7] = 0x595959;
		return 1;
	});
	def(0x308, "select_press_offset", [](Args &a) {
		SelectWindow *s = Object::as<SelectWindow>(a.u(0));
		if (!s) return 0;
		s->press_dx = a.i(1);
		s->press_dy = a.i(2);
		return 1;
	});
	def(0x309, "select_add", [](Args &a) {
		SelectWindow *s = Object::as<SelectWindow>(a.u(0));
		if (!s) return 4;
		bool disabled = a.i(3) != -1 && a.i(3) != 0;
		return s->add_item(a.s(1), disabled);
	});
	def(0x30a, "select_highlight", [](Args &a) {
		SelectWindow *s = Object::as<SelectWindow>(a.u(0));
		return s ? s->highlight(a.i(1), uint8_t(a.u(2)) != 0) : -1;
	});
	def(0x30b, "select_activate", [&e](Args &a) {
		e.text->active_select = 0;
		if (Object::as<SelectWindow>(a.u(0))) {
			e.text->active_select = a.u(0);
			return 0;
		}
		return 1;
	});
	def(0x30c, "select_copy_active", [&e](Args &a) {
		SelectWindow *s = Object::as<SelectWindow>(a.u(0));
		if (!s) return -1;
		SelectWindow *act = Object::as<SelectWindow>(e.text->active_select);
		if (!act) return 1;
		s->mark_chosen = act->mark_chosen;
		return 0;
	});
}

}  // namespace ore
