// System natives (docs/re/10_lifecycle_system.md §7.1-7.5, 7.8).
#include <cstdlib>
#include <cstring>

#include "core/args.h"
#include "core/engine.h"
#include "util/log.h"
#include "util/text.h"

namespace ore {
namespace {

// wvsprintfA over 32-bit argument words (%d %u %x %X %c %s with flags/width).
std::string mini_wsprintf(Vm &vm, const char *fmt, uint32_t args) {
	std::string out;
	int k = 0;
	for (const char *p = fmt; *p; ++p) {
		if (*p != '%') {
			out.push_back(*p);
			continue;
		}
		if (p[1] == '%') {
			out.push_back('%');
			++p;
			continue;
		}
		std::string spec = "%";
		++p;
		while (*p && strchr("-+ #0", *p)) spec.push_back(*p++);
		while (*p && *p >= '0' && *p <= '9') spec.push_back(*p++);
		if (*p == '.') {
			spec.push_back(*p++);
			while (*p && *p >= '0' && *p <= '9') spec.push_back(*p++);
		}
		if (*p == 'l' || *p == 'h') ++p;
		if (!*p) break;
		uint32_t v = vm.mem().rd32(args + uint32_t(k++) * 4);
		char buf[512];
		switch (*p) {
		case 'd':
		case 'i':
			spec += "d";
			snprintf(buf, sizeof buf, spec.c_str(), int32_t(v));
			break;
		case 'u':
		case 'x':
		case 'X':
			spec.push_back(*p);
			snprintf(buf, sizeof buf, spec.c_str(), v);
			break;
		case 'c':
			spec += "c";
			snprintf(buf, sizeof buf, spec.c_str(), int(v & 0xff));
			break;
		case 's':
			spec += "s";
			snprintf(buf, sizeof buf, spec.c_str(), v ? vm.mem().cstr(v) : "(null)");
			break;
		default:
			buf[0] = 0;
			break;
		}
		out += buf;
	}
	return out;
}

// N_4/N_5 index → app field (byte unless noted).
int flag_offset(int idx) {
	static const int map[] = {0xd74, 0xe2d, 0xd77, 0xd75, 0xe2f, 0xe31, 0xe33, 0xee4, 0xee5, 0xee6, 0xee7, 0xee8, 0xef0, 0xe36};
	return idx >= 0 && idx < 14 ? map[idx] : -1;
}

// N_8/N_9: (group, index) → (option offset, size) or (-1).
struct OptField {
	int off;
	int size;  // 1, 4, or 32 (font face buffer)
};
OptField option_field(int g, int i) {
	switch (g) {
	case 0:
		if (i >= 0 && i <= 6) return {0xe43 + i, 1};
		if (i >= 8 && i <= 11) return {0xe4a + (i - 8), 1};
		if (i == 12) return {0xe50, 4};
		if (i == 13) return {0xe54, 1};
		break;
	case 1:
		if (i >= 0 && i <= 6) return {0xe55 + i, 1};
		if (i == 7) return {0xe5c, 4};
		break;
	case 2:
		if (i == 0) return {0xe60, 32};
		if (i >= 1 && i <= 7) return {0xe80 + (i - 1), 1};
		if (i == 8) return {0xe88, 4};
		break;
	case 3:
		if (i >= 0 && i <= 4) return {0xe8c + i, 1};
		if (i >= 5 && i <= 8) return {0xe94 + (i - 5) * 4, 4};
		if (i >= 9 && i <= 14) return {0xea4 + (i - 9), 1};
		if (i == 15) return {0xe91, 1};
		break;
	case 4:
		if (i == 0) return {0xeb8, 1};
		if (i == 1) return {0xeb9, 1};
		if (i >= 2 && i < 16) return {0xeaa + (i - 2), 1};
		break;
	case 5:
		if (i == 0) return {0xeba, 1};
		if (i >= 1 && i <= 5) return {0xec9 + (i - 1), 1};
		if (i >= 6 && i < 20) return {0xebb + (i - 6), 1};
		break;
	case 6:
		if (i >= 0 && i <= 4) return {0xece + i, 1};
		if (i == 5) return {0xed4, 4};
		if (i >= 6 && i <= 8) return {0xed8 + (i - 6), 1};
		break;
	case 7:
		if (i == 0) return {0xede, 1};
		break;
	case 8:
		if (i >= 0 && i <= 3) return {0xedf + i, 1};
		break;
	}
	return {-1, 0};
}

}  // namespace

void register_system_natives(Engine &e) {
	Vm &vm = e.vm;
	auto def = [&vm](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};

	// ---- script control
	def(0x0, "call_script", [&e](Args &a) { e.vm.request_call(a.s(0)); return 0; });
	def(0x1, "goto_script", [&e](Args &a) { e.vm.request_goto(a.s(0)); return 0; });
	def(0x2, "stop_scripts", [&e](Args &) { e.vm.request_stop(); return 0; });

	// ---- window / environment
	def(0x3, "screen_mode_flags", [](Args &) { return 0; });
	def(0x4, "set_app_flag", [&e](Args &a) {
		e.app_flags[a.i(0)] = uint8_t(a.u(1));
		return 0;
	});
	def(0x5, "get_app_flag", [&e](Args &a) {
		int idx = a.i(0);
		if (idx == 12) return 0;  // install type: hard-disk install, no DVD
		auto it = e.app_flags.find(idx);
		if (it != e.app_flags.end()) return int(it->second);
		static const uint8_t defaults[] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0};
		return idx >= 0 && idx < 14 ? int(defaults[idx]) : 0;
	});
	(void)flag_offset;
	def(0x6, "system_info", [](Args &a) {
		switch (a.i(0)) {
		case 9: return 1;    // windowed
		case 14: return 7;   // all fullscreen depths supported
		case 21: return 1;   // tvpgl.dll loaded
		default: return 0;
		}
	});
	def(0x7, "query_string", [&e](Args &a) {
		int which = a.i(0);
		std::string v;
		bool null = false;
		if (which == 0)
			v = "{793485B9-B82A-4efc-A900-ABCC8D3FA43D}";
		else if (which == 2)
			v = e.platform.save_dir() + "\\";
		else if (which == 1 || which == 3)
			null = true;
		else
			return -1;
		uint32_t max = a.u(2);
		if (max == 0) return null ? 0 : int(v.size() + 1);
		a.out_str(1, null ? std::string() : v, max);
		return 0;
	});
	def(0x8, "set_option", [&e](Args &a) {
		int g = a.i(0), i = a.i(1);
		if (g == 0 && i == 7) return 1;
		OptField f = option_field(g, i);
		if (f.off < 0) return -1;
		if (f.size == 1)
			e.set_opt8(f.off, uint8_t(a.u(2)));
		else if (f.size == 4)
			e.set_opt32(f.off, a.u(2));
		else {
			std::string s = a.s(2);
			if (s.size() > 31) s.resize(31);
			uint8_t *d = e.vm.mem().host(e.opt_addr(f.off), 32);
			memset(d, 0, 32);
			memcpy(d, s.data(), s.size());
		}
		return 0;
	});
	def(0x9, "get_option", [&e](Args &a) -> int32_t {
		OptField f = option_field(a.i(0), uint8_t(a.u(1)));
		if (f.off < 0) return 0;
		if (f.size == 1) return e.opt8(f.off);
		if (f.size == 4) return int32_t(e.opt32(f.off));
		return int32_t(e.opt_addr(f.off));
	});
	def(0xa, "set_voice_char", [&e](Args &a) {
		auto &chars = e.sound.voice_info().characters;
		int g = a.i(0);
		if (g < 0 || g >= int(chars.size())) return -1;
		switch (a.i(1)) {
		case 0: chars[size_t(g)].volume = a.i(2); break;
		case 1: chars[size_t(g)].enabled = a.b(2); break;
		case 2: chars[size_t(g)].flag = a.b(2); break;
		default: return -1;
		}
		return 0;
	});
	def(0xb, "get_voice_char", [&e](Args &a) -> int32_t {
		auto &vi = e.sound.voice_info();
		int g = a.i(0);
		if (g < 0 || g >= int(vi.characters.size())) return int32_t(vi.characters.size());
		VoiceCharacter &c = vi.characters[size_t(g)];
		// strings are handed out as VM pointers; keep one cached copy per character/field
		static std::map<std::pair<int, int>, uint32_t> cache;
		auto str = [&](int field, const std::string &s) -> int32_t {
			auto key = std::make_pair(g, field);
			auto it = cache.find(key);
			if (it != cache.end()) e.vm.mem().free(it->second);
			uint32_t p = e.vm.mem().alloc_string(s, "voice_name");
			cache[key] = p;
			return int32_t(p);
		};
		switch (a.i(1)) {
		case 0: return c.volume;
		case 1: return c.enabled;
		case 2: return c.flag;
		case 3: return str(3, c.name);
		case 4: return c.name2.empty() ? 0 : str(4, c.name2);
		case 5: return str(5, c.flag ? (c.name2.empty() ? std::string("\x81\x48\x81\x48\x81\x48") : c.name2) : c.name);
		case 6: return c.prefixes.empty() ? 0 : str(6, c.prefixes[0]);
		}
		return 0;
	});
	def(0xd, "change_display_mode", [](Args &) { return 0; });
	def(0x13, "enter_busy", [&e](Args &) { return e.busy++ ? 1 : 0; });
	def(0x14, "leave_busy", [&e](Args &) {
		int old = e.busy;
		if (e.busy > 0) --e.busy;
		return old;
	});
	def(0x17, "set_frame_skip", [](Args &) { return 0; });
	def(0x18, "window_command", [&e](Args &a) {
		if (a.i(0) == 3) e.request_quit();
		return a.i(0) == 3 ? 1 : 0;
	});
	def(0x19, "open_dialog", [](Args &) { return 0; });

	// ---- strings and helpers
	def(0x39, "strcpy", [&e](Args &a) -> int32_t {
		std::string s = a.s(1);
		a.out_str(0, s);
		return int32_t(a.u(0));
	});
	def(0x3a, "strncpy_from", [](Args &a) {
		std::string s = a.s(1);
		size_t off = a.u(2), max = a.u(3);
		std::string part = off < s.size() ? s.substr(off, max) : std::string();
		a.out_str(0, part);
		return 0;
	});
	def(0x3c, "strcat", [](Args &a) -> int32_t {
		std::string s = std::string(a.s(0)) + a.s(1);
		a.out_str(0, s);
		return int32_t(a.u(0));
	});
	def(0x3e, "strcmp", [](Args &a) { return strcmp(a.s(0), a.s(1)); });
	def(0x3f, "stricmp", [](Args &a) { return strcasecmp(a.s(0), a.s(1)); });
	def(0x41, "wsprintf", [&e](Args &a) {
		a.out_str(0, mini_wsprintf(e.vm, a.s(1), a.addr(2)));
		return 0;
	});
	def(0x43, "atof", [](Args &a) -> int32_t {
		if (a.null(0)) return 0;
		float f = float(atof(a.s(0)));
		int32_t bits;
		memcpy(&bits, &f, 4);
		return bits;
	});
	def(0x44, "strupr", [](Args &a) {
		a.out_str(0, to_upper_ascii(a.s(0)));
		return 0;
	});
	def(0x4c, "fill_dwords", [&e](Args &a) {
		uint32_t dst = a.u(0);
		if (!dst) return 0;
		for (uint32_t i = 0; i < a.u(2); ++i) e.vm.mem().wr32(dst + i * 4, a.u(1));
		return 1;
	});
	def(0x4e, "memcpy", [&e](Args &a) {
		uint32_t dst = a.u(0), src = a.u(1), n = a.u(2);
		if (!dst || !src) return 0;
		memmove(e.vm.mem().host(dst, n), e.vm.mem().host(src, n), n);
		return 1;
	});
	def(0x52, "random", [&e](Args &a) {
		uint32_t n = a.u(0);
		if (!n) return 0;
		// xorshift32; the original uses Knuth's subtractive generator
		uint32_t x = e.rng_state;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		e.rng_state = x;
		return int32_t(x % n);
	});

	// ---- file system
	def(0x5c, "add_search_dir", [](Args &) { return 0; });
	def(0x61, "find_disc", [&e](Args &a) {
		a.out_str(1, e.config.data_dir);
		return 0;
	});
	def(0x62, "loose_file_size", [](Args &a) {
		a.out(1, 0);
		return 0;
	});
	def(0x66, "format_control", [](Args &) { return 0; });
	def(0x67, "mount_archive", [](Args &) { return 0; });  // all archives are mounted at boot

	// ---- message boxes: wait for the platform's answer
	auto dialog = [&e](Args &a, const char *text, int kind) -> int32_t {
		static bool open = false;
		if (!open) {
			e.platform.begin_dialog(e.platform.to_utf8(text, e.encoding), kind);
			open = true;
		}
		int r = e.platform.poll_dialog();
		if (r < 0) {
			e.vm.retry_native();
			return 0;
		}
		open = false;
		(void)a;
		return r;
	};
	def(0x311, "message_box", [dialog](Args &a) { return dialog(a, a.s(1), 0) * 0; });
	def(0x313, "question_box", [dialog](Args &a) { return dialog(a, a.s(1), 1); });
	def(0x314, "dialog", [dialog](Args &a) { return dialog(a, a.s(2), a.b(0) ? 1 : 0); });
}

}  // namespace ore
