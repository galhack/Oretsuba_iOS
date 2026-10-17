// Sound, movie and input natives (docs/re/50_audio_movie_input.md §4).
#include <cmath>
#include <cstring>

#include "core/args.h"
#include "core/engine.h"
#include "text/text_system.h"
#include "util/log.h"

namespace ore {
namespace {

// Loose sound objects are tagged so they cannot be confused with other handles.
constexpr int32_t kSoundTag = 0x50000000;
int obj_id(uint32_t h) {
	return (h & 0xF0000000u) == uint32_t(kSoundTag) ? int(h & 0x0FFFFFFF) : 0;
}
int32_t obj_handle(int id) {
	return id ? (kSoundTag | id) : 0;
}

// N_294 / N_2a3 export the BGM stream / a channel's sound as objects.
constexpr int32_t kBgmObj = 0x5FFF0000;
constexpr int32_t kChanObj = 0x5FFE0000;

float movie_gain(int v) {
	if (v < 1) return 0.0f;
	double db = -std::pow(17.2 * (1.0 - v / 256.0), 3.0) / 100.0;
	return float(std::pow(10.0, db / 20.0));
}

}  // namespace

void register_audio_natives(Engine &e) {
	auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		e.vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};
	SoundManager &sm = e.sound;
	auto str = [](Args &a, int i) { return a.null(i) ? std::string() : a.str(i); };

	// ---- loose sound objects
	def(0x253, "snd_free_all", [&sm](Args &) { sm.obj_free_all(); return 0; });
	def(0x254, "snd_new", [&sm, str](Args &a) { return obj_handle(sm.obj_new_static(str(a, 0), a.i(1))); });
	def(0x255, "snd_dup", [&sm](Args &a) { return obj_handle(sm.obj_duplicate(obj_id(a.u(0)))); });
	def(0x256, "snd_new_stream", [&sm, str](Args &a) { return obj_handle(sm.obj_new_stream(str(a, 0), std::string())); });
	def(0x257, "snd_new_stream2", [&sm, str](Args &a) { return obj_handle(sm.obj_new_stream(str(a, 0), str(a, 1))); });
	def(0x258, "snd_delete", [&e, &sm](Args &a) {
		uint32_t p = a.u(0);
		if (!p) return 0;
		sm.obj_delete(obj_id(e.vm.mem().rd32(p)));
		e.vm.mem().wr32(p, 0);
		return 0;
	});
	def(0x259, "snd_play", [&sm](Args &a) { sm.obj_play(obj_id(a.u(0)), false); return 0; });
	def(0x25a, "snd_play_loop", [&sm](Args &a) { sm.obj_play(obj_id(a.u(0)), true); return 0; });
	def(0x25d, "snd_stop", [&sm](Args &a) { sm.obj_stop(obj_id(a.u(0))); return 0; });
	def(0x265, "snd_length", [&sm](Args &a) -> int32_t {
		uint32_t h = a.u(0);
		if (h == uint32_t(kBgmObj)) return sm.bgm_length_ms();
		if ((h & 0xFFFF0000u) == uint32_t(kChanObj)) return sm.channel_length_ms(int(h & 0xFFFF));
		return sm.obj_length_ms(obj_id(h));
	});
	def(0x266, "snd_position", [&sm](Args &a) -> int32_t {
		uint32_t h = a.u(0);
		if ((h & 0xFFFF0000u) == uint32_t(kChanObj)) return sm.channel_position_ms(int(h & 0xFFFF));
		return 0;
	});
	def(0x268, "snd_volume", [&sm](Args &a) { sm.obj_set_volume(obj_id(a.u(0)), a.i(1)); return 0; });
	def(0x26e, "snd_lip_level", [&sm](Args &a) -> int32_t {
		uint32_t h = a.u(0);
		if ((h & 0xFFFF0000u) == uint32_t(kChanObj)) return sm.channel_lip_level(int(h & 0xFFFF), 0);
		return 0;
	});
	def(0x26f, "snd_lip_level_n", [&sm](Args &a) -> int32_t {
		uint32_t h = a.u(0);
		if ((h & 0xFFFF0000u) == uint32_t(kChanObj)) return sm.channel_lip_level(int(h & 0xFFFF), a.i(2));
		return 0;
	});

	// ---- sound manager
	def(0x285, "sm_switch", [&sm](Args &a) {
		int type = a.i(0), x = a.i(1), y = a.i(2);
		switch (type) {
		case 0: sm.set_bgm_enabled(x != 0); break;
		case 1: sm.set_se_enabled(x != 0, y); break;
		case 2:
			if (x == 0) sm.set_category_enabled(1, y != 0);
			if (x == 1) sm.set_category_enabled(2, y != 0);
			if (x == 2) sm.set_category_enabled(4, y != 0);
			break;
		case 3: sm.set_category_default_volume(x, y); break;
		}
		return 0;
	});
	def(0x286, "sm_get", [&sm](Args &a) -> int32_t {
		int type = a.i(0), x = a.i(1);
		if (type == 0) return sm.bgm_enabled();
		if (type == 1) return sm.category_enabled(1);
		if (type == 2) return sm.category_enabled(x == 0 ? 1 : x == 1 ? 2 : 4);
		if (type == 3) return sm.category_default_volume(x);
		return 0;
	});
	def(0x287, "sm_channels_volume", [&sm](Args &a) { sm.set_channels_volume(a.i(0), a.b(1), a.i(2)); return 0; });
	def(0x288, "sm_bgm_volume", [&sm](Args &a) { sm.set_bgm_volumes(a.i(0), a.i(1)); return 0; });
	def(0x289, "sm_bgm_pan", [&sm](Args &a) { sm.set_bgm_pan(a.i(0)); return 0; });
	def(0x28a, "bgm_play", [&sm, str](Args &a) {
		if (a.u(1) == 0xffff) return sm.bgm_play(str(a, 0), true, std::string());
		return sm.bgm_play(str(a, 0), a.b(1), str(a, 2));
	});
	def(0x28b, "bgm_stop", [&sm](Args &) { sm.bgm_stop(); return 0; });
	def(0x28c, "bgm_pause", [&sm](Args &) { sm.bgm_pause(true); return 0; });
	def(0x28d, "bgm_resume", [&sm](Args &) { sm.bgm_pause(false); return 0; });
	def(0x28e, "bgm_fade_volume", [&sm](Args &a) { sm.bgm_fade_volume(a.i(0), a.i(1)); return 0; });
	def(0x28f, "bgm_fade_in", [&sm, str](Args &a) { return sm.bgm_fade_in(a.i(0), str(a, 1), a.b(2), str(a, 3)); });
	def(0x290, "bgm_fade_out", [&sm](Args &a) { return sm.bgm_fade_out(a.i(0)); });
	def(0x291, "bgm_crossfade", [&sm, str](Args &a) {
		bool short_form = a.u(2) == 0xffff;
		int overlap = a.u(4) == 0xffff ? a.i(0) : a.i(4);
		return sm.bgm_crossfade(a.i(0), str(a, 1), short_form ? true : a.b(2), short_form ? std::string() : str(a, 3),
		                        overlap, a.i(5));
	});
	def(0x292, "bgm_playing", [&sm](Args &) { return sm.bgm_playing_count(); });
	def(0x293, "bgm_fading", [&sm](Args &) { return sm.bgm_fade_type(); });
	def(0x294, "bgm_object", [&sm](Args &) { return sm.bgm_playing_count() ? kBgmObj : 0; });
	def(0x295, "sm_alloc_channels", [&sm](Args &a) { sm.allocate_channels(a.i(0)); return 0; });
	def(0x296, "ch_volume", [&sm](Args &a) { sm.set_channel_volume(a.i(0), a.i(1), a.i(2)); return 0; });
	def(0x297, "ch_pan", [&sm](Args &a) { sm.set_channel_pan(a.i(0), a.i(1)); return 0; });
	def(0x298, "ch_category", [&sm](Args &a) { sm.set_channel_category(a.i(0), a.i(1)); return 0; });
	def(0x299, "ch_load", [&sm, str](Args &a) { return sm.load(a.i(0), str(a, 1), false); });
	def(0x29a, "ch_load_stream", [&sm, str](Args &a) { return sm.load(a.i(0), str(a, 1), true); });
	def(0x29b, "ch_unload", [&sm](Args &a) { sm.unload(a.i(0)); return 0; });
	def(0x29c, "ch_play", [&sm, &e](Args &a) {
		// The skip key suppresses SE in the original via script checks; play normally here.
		(void)e;
		return sm.play(a.i(0), a.b(1));
	});
	def(0x29d, "ch_stop", [&sm](Args &a) { sm.stop(a.i(0)); return 0; });
	def(0x29e, "ch_fade_volume", [&sm](Args &a) { sm.fade_channel_volume(a.i(0), a.i(1), a.i(2)); return 0; });
	def(0x29f, "ch_fade_in", [&sm, str](Args &a) {
		sm.fade_in(a.i(0), str(a, 1), a.i(2), a.b(3), a.b(4));
		return 0;
	});
	def(0x2a0, "ch_fade_out", [&sm](Args &a) { sm.fade_out(a.i(0), a.i(1)); return 0; });
	def(0x2a1, "ch_playing", [&sm](Args &a) { return sm.channel_playing(a.i(0)) ? 1 : 0; });
	def(0x2a2, "ch_fade_state", [&sm](Args &a) { return sm.channel_fade_state(a.i(0)); });
	def(0x2a3, "ch_object", [&sm](Args &a) -> int32_t {
		int ch = a.i(0);
		return sm.channel_length_ms(ch) > 0 || sm.channel_playing(ch) ? (kChanObj | ch) : 0;
	});
	def(0x2a4, "ch_load_voice", [&sm, &e](Args &a) {
		int n = a.i(2);
		std::vector<int> list;
		for (int i = 0; i < n; ++i) list.push_back(int32_t(e.vm.mem().rd32(a.addr(3) + uint32_t(i) * 4)));
		int r = sm.load_voice(a.i(0), a.null(1) ? std::string() : a.str(1), list);
		if (r >= -1 && a.i(0) >= 0 && e.opt8(0xe48)) {
			// lip-sync analysis was requested by load_voice when the slot is known
		}
		return r;
	});
	def(0x2a5, "bgm_duck", [&sm, &e](Args &a) {
		sm.duck(a.i(0), a.null(1) ? nullptr : a.s(1), e.opt8(0xea9));
		return 0;
	});

	// ---- movies
	def(0x271, "movie_open", [&e](Args &a) {
		e.movie.reset();
		e.movie_state = 3;
		std::string name = a.s(0);
		Vfs::Location loc;
		if (!e.vfs.locate(name, loc)) {
			log_warn("movie: missing %s", name.c_str());
			return 1;
		}
		std::unique_ptr<ByteSource> src;
		if (loc.archive) {
			LpkArchive::StreamInfo info;
			if (loc.archive->stream_info(loc.entry, info)) src = ByteSource::from_lpk(info);
		} else {
			src = ByteSource::from_file(loc.host_path);
		}
		auto mp = std::make_unique<MoviePlayer>();
		std::string err;
		if (!src || !mp->open(std::move(src), &err)) {
			log_warn("movie: %s", err.c_str());
			return 2;
		}
		e.movie = std::move(mp);
		e.movie_name = name;
		e.movie_fullscreen = true;  // both paths show the movie over the whole game view
		e.movie_rect = Rect(0, 0, Engine::kWidth, Engine::kHeight);
		log_info("movie: opened %s (%dx%d)", name.c_str(), e.movie->width(), e.movie->height());
		return 0;
	});
	def(0x272, "movie_close", [&e](Args &) {
		if (e.movie) log_info("movie: closed %s, finished=%d", e.movie_name.c_str(), e.movie->finished());
		if (e.movie) e.movie->stop();
		e.movie.reset();
		e.movie_state = 0;
		gfx_touch();
		return 0;
	});
	def(0x273, "movie_loop", [](Args &) { return 0; });
	def(0x274, "movie_play", [&e](Args &) {
		if (!e.movie) return 1;
		e.movie->start(&e.mixer, movie_gain(e.movie_volume));
		e.movie_state = 2;
		return 0;
	});
	def(0x275, "movie_stop", [&e](Args &) {
		if (e.movie) e.movie->stop();
		e.movie_state = 0;
		return 0;
	});
	def(0x276, "movie_pause", [&e](Args &) { if (e.movie_state == 2) e.movie_state = 1; return 0; });
	def(0x277, "movie_resume", [&e](Args &) { if (e.movie_state == 1) e.movie_state = 2; return 0; });
	def(0x278, "movie_rect", [&e](Args &a) {
		e.movie_rect = Rect(a.i(0), a.i(1), a.i(2), a.i(3));
		return 0;
	});
	def(0x27e, "movie_state", [&e](Args &) {
		if (e.movie && e.movie->finished()) e.movie_state = 0;
		return e.movie ? e.movie_state : 0;
	});
	def(0x27f, "movie_volume", [&e](Args &a) {
		e.movie_volume = a.i(0);
		return 0;
	});
}

void register_input_natives(Engine &e) {
	auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		e.vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};
	Input &in = e.input;
	// Logical buttons (app+0x1904): 0 up 1 down 2 left 3 right 4 decide 5 cancel,
	// 6..17 key-config, 18 PgUp, 19 PgDn.
	auto button = [&e, &in](int b, bool edge) -> bool {
		if (edge && b >= 6 && b <= 17) {
			// FUNC_LIB remaps shortcut indices through G[868] to button-bar
			// commands. Native controls send those semantic commands directly.
			uint32_t entry = e.vm.global_addr(868) + uint32_t(b - 6) * 4;
			if (e.vm.mem().valid(entry, 4) && in.command_pressed(int(e.vm.mem().rd32(entry)))) return true;
		}
		auto key = [&](int dik) { return edge ? in.key_pressed(dik) : in.key_held(dik); };
		auto mouse = [&](int code) { return edge ? in.mouse_pressed(code) : in.mouse_held(code); };
		switch (b) {
		case 0: return key(0xC8);
		case 1: return key(0xD0);
		case 2: return key(0xCB);
		case 3: return key(0xCD);
		case 4: return key(0x1C) || key(0x9C) || (e.opt8(0xeaa) && key(e.opt8(0xeaa)));
		case 5: return key(0x01) || (e.opt8(0xeab) && key(e.opt8(0xeab)));
		case 18: return key(0xC9) || (e.opt8(0xeca) && mouse(3));
		case 19: return key(0xD1) || (e.opt8(0xecd) && mouse(4));
		default:
			break;
		}
		if (b >= 6 && b <= 17) {
			int k = e.opt8(0xeac + (b - 6));
			if (k && key(k)) return true;
		}
		// mouse right / middle / wheel-up mappings (config 5,1 / 5,4 / 5,2)
		static const int table[] = {-1, 0xc, 0xb, 6, 0xf, 7, 8};
		int r = e.opt8(0xec9), m = e.opt8(0xecc);
		if (r >= 1 && r <= 6 && table[r] == b && mouse(1)) return true;
		if (m >= 1 && m <= 6 && table[m] == b && mouse(2)) return true;
		if (!e.opt8(0xeca) && b == 0xe && mouse(3)) return true;
		return false;
	};
	auto skip_key = [&e, &in](bool edge) {
		auto key = [&](int dik) { return edge ? in.key_pressed(dik) : in.key_held(dik); };
		if (e.opt8(0xeb8)) return key(0x1D) || key(0x9D);
		return key(0x2A) || key(0x36);
	};
	def(0x220, "button_pressed", [button](Args &a) { return button(a.i(0), true) ? 1 : 0; });
	def(0x221, "button_held", [button](Args &a) { return button(a.i(0), false) ? 1 : 0; });
	def(0x222, "first_button", [button](Args &a) {
		int first = a.i(0), count = a.i(1);
		int last = count ? first + count : 20;
		for (int b = first; b < last && b < 20; ++b)
			if (button(b, true)) return b;
		return -1;
	});
	def(0x227, "decide_pressed", [button](Args &) { return button(4, true) ? 1 : 0; });
	def(0x228, "cancel_pressed", [button](Args &) { return button(5, true) ? 1 : 0; });
	for (int k = 0; k < 4; ++k) {
		def(0x229 + k, "dir_pressed", [button, k](Args &) { return button(k, true) ? 1 : 0; });
		def(0x22d + k, "dir_held", [button, k](Args &) { return button(k, false) ? 1 : 0; });
	}
	def(0x233, "skip_key_pressed", [skip_key](Args &) { return skip_key(true) ? 1 : 0; });
	def(0x234, "skip_key_held", [skip_key](Args &) { return skip_key(false) ? 1 : 0; });
	def(0x235, "key_pressed", [&in](Args &a) { return in.key_pressed(a.i(0)) ? 1 : 0; });
	def(0x236, "key_held", [&in](Args &a) { return in.key_held(a.i(0)) ? 1 : 0; });
	def(0x23c, "cursor_shape", [](Args &) { return 0; });
	def(0x23e, "cursor_show", [](Args &) { return 0; });
	def(0x23f, "cursor_pos", [&in](Args &a) {
		a.out(0, in.cursor_x());
		a.out(1, in.cursor_y());
		return 0;
	});
	def(0x240, "cursor_warp", [&in](Args &a) {
		in.warp(a.i(0), a.i(1));
		return 0;
	});
	def(0x241, "cursor_edge", [&in](Args &) { return in.edge_code(); });
	def(0x242, "left_click", [&in](Args &) { return in.mouse_pressed(0) ? 1 : 0; });
	def(0x243, "right_click", [&in](Args &) { return in.mouse_pressed(1) ? 1 : 0; });
	def(0x244, "middle_click", [&in](Args &) { return in.mouse_pressed(2) ? 1 : 0; });
	def(0x245, "left_held", [&in](Args &) { return in.mouse_held(0) ? 1 : 0; });
	def(0x246, "right_held", [&in](Args &) { return in.mouse_held(1) ? 1 : 0; });
	def(0x247, "middle_held", [&in](Args &) { return in.mouse_held(2) ? 1 : 0; });
	def(0x248, "wheel_up", [&in](Args &a) {
		a.out(0, in.wheel());
		return in.mouse_pressed(3) ? 1 : 0;
	});
	def(0x249, "wheel_down", [&in](Args &a) {
		a.out(0, in.wheel());
		return in.mouse_pressed(4) ? 1 : 0;
	});
	def(0x24d, "window_command_9c52", [](Args &) { return 0; });

	// stopwatches (N_218..N_21a)
	def(0x218, "stopwatch_alloc", [&e](Args &a) {
		e.stopwatches.assign(size_t(a.u(0) & 0xffff), 0);
		return 0;
	});
	def(0x219, "stopwatch_start", [&e](Args &a) {
		size_t i = a.u(0) & 0xffff;
		if (i >= e.stopwatches.size()) return 0;
		e.stopwatches[i] = e.platform.now_ms();
		return 1;
	});
	def(0x21a, "stopwatch_elapsed", [&e](Args &a) -> int32_t {
		size_t i = a.u(0) & 0xffff;
		if (i >= e.stopwatches.size()) return 0;
		return int32_t(e.platform.now_ms() - e.stopwatches[i]);
	});
}

}  // namespace ore
