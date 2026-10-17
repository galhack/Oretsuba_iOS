// Save system (docs/re/10_lifecycle_system.md §5.4, §6, §7.3): user-variable
// copies, DATAbbss.SAV files, GAMEDATA.DAT, TEXT_LOG.DAT and the in-memory
// snapshot. File layouts follow the original so PC saves can be imported.
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "core/args.h"
#include "core/engine.h"
#include "gfx/blend.h"
#include "text/text_system.h"
#include "util/bytes.h"
#include "util/log.h"

namespace ore {
namespace {

bool read_save_file(const std::string &path, std::vector<uint8_t> &out) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(size_t(n));
	bool ok = n == 0 || fread(out.data(), 1, size_t(n), f) == size_t(n);
	fclose(f);
	return ok;
}

bool write_file(const std::string &path, const std::vector<uint8_t> &data) {
	std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = fclose(f) == 0 && ok;
	if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
	return ok;
}

uint8_t checksum(const uint8_t *p, size_t n) {
	uint8_t s = 0;
	for (size_t i = 0; i < n; ++i) s = uint8_t(s + p[i]);
	return s;
}

// Matches the scope/type selectors of the variable-copy natives.
bool var_matches(const Vm::Global &g, int scope_sel, int type_sel) {
	switch (scope_sel) {
	case 1: if (g.flags != 0) return false; break;
	case 2: if (g.flags != 1) return false; break;
	case 3: if (g.flags != 2) return false; break;
	default: break;
	}
	switch (type_sel) {
	case 1: return g.type == 0;
	case 2: return g.type == 1;
	case 3: return g.type == 2;
	default: return true;
	}
}

std::string save_file_name(int bank, int slot) {
	char buf[32];
	snprintf(buf, sizeof buf, "DATA%02d%02d.SAV", bank, slot);
	return buf;
}

constexpr size_t kThumbW = 100, kThumbH = 75;
constexpr size_t kTitleAt = 0x57EC, kPreviewAt = 0x5818, kHeaderAt = 0x58A0;

}  // namespace

void Engine::copy_vars(int from, int to, int scope_sel, int type_sel) {
	const auto &gl = vm.globals();
	for (size_t i = 0; i < gl.size(); ++i) {
		const Vm::Global &g = gl[i];
		if (!var_matches(g, scope_sel, type_sel)) continue;
		uint32_t n = g.extra;
		if (!n) continue;
		uint32_t s = vm.global_data(int(i), from), d = vm.global_data(int(i), to);
		memmove(vm.mem().host(d, n), vm.mem().host(s, n), n);
	}
}

// ---------------------------------------------------------------- state blob

std::vector<uint8_t> Engine::state_blob(bool with_log) {
	// refresh copy2 with the live values of the N_28-registered vars
	for (int idx : live_save_vars) {
		const Vm::Global &g = vm.globals()[size_t(idx)];
		uint32_t n = g.extra;
		memmove(vm.mem().host(vm.global_data(idx, 2), n), vm.mem().host(vm.global_data(idx, 0), n), n);
	}
	ByteWriter w;
	w.u8(0);
	const auto &gl = vm.globals();
	for (size_t i = 0; i < gl.size(); ++i) {
		const Vm::Global &g = gl[i];
		if (g.flags != 1 || g.type >= 2) continue;
		w.u8(g.type);
		const uint8_t *p = vm.mem().host(vm.global_data(int(i), 2), g.extra);
		w.bytes(std::vector<uint8_t>(p, p + g.extra));
	}
	bool has_log = with_log && opt8(0xe5b);
	w.u8(has_log);
	if (has_log) {
		std::vector<uint8_t> log = text ? text->save_backlog() : std::vector<uint8_t>();
		w.blob(log);
		w.blob(has_mem_snapshot ? mem_snapshot : std::vector<uint8_t>());
	}
	return w.take();
}

int Engine::restore_state_blob(const std::vector<uint8_t> &blob) {
	ByteReader r(blob);
	r.u8();
	const auto &gl = vm.globals();
	for (size_t i = 0; i < gl.size(); ++i) {
		const Vm::Global &g = gl[i];
		if (g.flags != 1 || g.type >= 2) continue;
		if (r.u8() != g.type) return 4;
		std::vector<uint8_t> v = r.bytes(g.extra);
		if (r.bad()) return 4;
		memcpy(vm.mem().host(vm.global_data(int(i), 0), g.extra), v.data(), g.extra);
	}
	if (text) text->clear_message_state();
	has_mem_snapshot = false;
	mem_snapshot.clear();
	bool has_log = r.u8();
	if (has_log && !r.bad()) {
		uint32_t n = r.u32();
		std::vector<uint8_t> log = r.bytes(n);
		if (text && opt8(0xe5b)) text->load_backlog(log);
		uint32_t m = r.u32();
		if (m && !r.bad()) {
			mem_snapshot = r.bytes(m);
			has_mem_snapshot = !r.bad();
		}
	}
	return 0;
}

// ---------------------------------------------------------------- save files

std::vector<uint8_t> Engine::thumbnail(int w, int h) {
	std::vector<uint8_t> out(size_t(w) * h * 3, 0);
	bool blank = thumb_flag_ptr[1] && vm.mem().rd32(thumb_flag_ptr[1]);
	if (blank) return out;
	ImageRef src;
	if (thumb_name_ptr) {
		std::string name = vm.mem().cstr(thumb_name_ptr);
		if (!name.empty() && name.find('\\') == std::string::npos) src = load_image("SYS\\_" + name + ".ELG", false);
	}
	if (!src && thumb_source) {
		if (Surface *s = surface_of(thumb_source)) {
			s->update(now);
			src = s->image_ref();
		} else {
			src = image_of(thumb_source);
		}
	}
	if (!src) src = std::make_shared<Image>(frame_);
	Image small(w, h, 32);
	stretch_blit(small, *src, 0, 0, w, h, src->rect(), 0, 2, 255, false);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			uint32_t p = small.row32(y)[x];
			uint8_t *d = &out[(size_t(y) * w + x) * 3];
			d[0] = uint8_t(p), d[1] = uint8_t(p >> 8), d[2] = uint8_t(p >> 16);
		}
	return out;
}

static int ring_slot(Engine &e, int bank, int slot, bool for_save) {
	if (slot >= 0) return slot;
	if (bank == 10 || bank == 11) {
		int base = bank == 10 ? 0 : 5, span = bank == 10 ? 10 : 5;
		int next_off = bank == 10 ? 0xedb : 0xedc;
		int next = e.opt8(next_off);
		if (bank == 11 && (next < 5 || next > 9)) next = 5;
		if (for_save) {
			int s = next;
			int n = s + 1;
			if (n >= base + span) n = base;
			e.set_opt8(next_off, uint8_t(n));
			e.set_opt8(bank == 10 ? 0xedf : 0xee0, uint8_t(s));
			return s;
		}
		// newest existing file walking backwards from the next index
		for (int k = 1; k <= span; ++k) {
			int s = next - k;
			if (s < base) s += span;
			struct stat st;
			if (stat(e.save_path(save_file_name(bank, s)).c_str(), &st) == 0) return s;
		}
		return -1;
	}
	return -1;
}

int Engine::save_game(int bank, int slot) {
	slot = ring_slot(*this, bank, slot, true);
	if (slot < 0) return 1;
	std::vector<uint8_t> file(kHeaderAt, 0);
	memcpy(file.data(), "OT01", 4);
	std::vector<uint8_t> th = thumbnail(int(kThumbW), int(kThumbH));
	memcpy(&file[8], th.data(), th.size());
	if (save_title_ptr) {
		std::string t = vm.mem().cstr(save_title_ptr);
		if (t.size() > 40) t.resize(40);
		memcpy(&file[kTitleAt], t.data(), t.size());
	}
	if (text && opt8(0xedd) && save_preview_flag_ptr && vm.mem().rd32(save_preview_flag_ptr)) {
		std::string p = text->preview_text(140);
		memcpy(&file[kPreviewAt], p.data(), std::min<size_t>(p.size(), 0x8f));
	}
	std::vector<uint8_t> blob = state_blob(true);
	ByteWriter w;
	w.bytes(file);
	w.u32(uint32_t(blob.size()));
	w.u32(0);  // stored raw
	w.u8(checksum(blob.data(), blob.size()));
	w.bytes(blob);
	if (!write_file(save_path(save_file_name(bank, slot)), w.data())) return 2;
	set_opt8(0xede, 1);
	save_options();
	write_gamedata();
	return 0;
}

int Engine::load_game(int bank, int slot) {
	slot = ring_slot(*this, bank, slot, false);
	if (slot < 0) return -1;
	std::vector<uint8_t> d;
	if (!read_save_file(save_path(save_file_name(bank, slot)), d)) return -1;
	if (d.size() < kHeaderAt + 9 || memcmp(d.data(), "OT01", 4) != 0) return 3;
	ByteReader r(d);
	r.seek(4);
	if (r.u32() != 0) return 5;
	r.seek(kHeaderAt);
	uint32_t stored = r.u32(), orig = r.u32();
	uint8_t sum = r.u8();
	std::vector<uint8_t> data = r.bytes(stored);
	if (r.bad() || checksum(data.data(), data.size()) != sum) return 3;
	if (orig) data = lzss_decompress(data.data(), data.size(), orig);
	return restore_state_blob(data);
}

// ---------------------------------------------------------------- GAMEDATA / TEXT_LOG

int Engine::write_gamedata() {
	if (gamedata_name.empty()) return 2;
	ByteWriter body;
	const auto &gl = vm.globals();
	for (size_t i = 0; i < gl.size(); ++i) {
		const Vm::Global &g = gl[i];
		if (!var_matches(g, 3, 0) || g.type >= 2) continue;
		body.u8(g.type);
		const uint8_t *p = vm.mem().host(vm.global_data(int(i), 0), g.extra);
		body.bytes(std::vector<uint8_t>(p, p + g.extra));
	}
	std::vector<uint8_t> b = body.take();
	ByteWriter w;
	w.u32(uint32_t(b.size()));
	w.u32(0);
	w.u32(0);  // extA
	w.u32(0);  // extB
	w.u8(1);
	w.u8(checksum(b.data(), b.size()));
	w.bytes(b);
	return write_file(save_path(gamedata_name), w.data()) ? 0 : 1;
}

int Engine::read_gamedata() {
	if (gamedata_name.empty()) return 1;
	std::vector<uint8_t> d;
	if (!read_save_file(save_path(gamedata_name), d)) return 1;
	ByteReader r(d);
	uint32_t stored = r.u32(), orig = r.u32();
	r.skip(r.u32());
	r.skip(r.u32());
	r.u8();
	uint8_t sum = r.u8();
	std::vector<uint8_t> body = r.bytes(stored);
	if (r.bad() || checksum(body.data(), body.size()) != sum) return 2;
	if (orig) body = lzss_decompress(body.data(), body.size(), orig);
	ByteReader b(body);
	const auto &gl = vm.globals();
	for (size_t i = 0; i < gl.size(); ++i) {
		const Vm::Global &g = gl[i];
		if (!var_matches(g, 3, 0) || g.type >= 2) continue;
		if (b.u8() != g.type) return 2;
		std::vector<uint8_t> v = b.bytes(g.extra);
		if (b.bad()) return 2;
		memcpy(vm.mem().host(vm.global_data(int(i), 0), g.extra), v.data(), g.extra);
	}
	return 0;
}

int Engine::write_textlog() {
	if (textlog_name.empty()) return 2;
	ByteWriter w;
	w.bytes({'T', 'L', 'O', 'G'});
	auto &chars = sound.voice_info().characters;
	w.u32(uint32_t(chars.size()));
	for (auto &c : chars) {
		w.u32(uint32_t(c.volume));
		w.u8(c.enabled);
		w.u8(c.flag);
	}
	ByteWriter s;
	auto &table = vm.init_table();
	for (auto &e : table) {
		s.u8(uint8_t(e.name.size() + 1));
		s.bytes(std::vector<uint8_t>(e.name.begin(), e.name.end()));
		s.u8(0);
		s.u32(e.count_a);
		s.u32(uint32_t(e.bits_a.size()));
		s.bytes(e.bits_a);
		s.u32(e.count_b);
		s.u32(uint32_t(e.bits_b.size()));
		s.bytes(e.bits_b);
	}
	std::vector<uint8_t> sb = s.take();
	w.u32(uint32_t(sb.size()));
	w.u32(0);
	w.u32(uint32_t(table.size()));
	w.bytes(sb);
	return write_file(save_path(textlog_name), w.data()) ? 0 : 1;
}

int Engine::read_textlog() {
	if (textlog_name.empty()) return 1;
	std::vector<uint8_t> d;
	if (!read_save_file(save_path(textlog_name), d)) return 1;
	ByteReader r(d);
	if (r.str(4) != "TLOG") return 2;
	auto &chars = sound.voice_info().characters;
	uint32_t n = r.u32();
	std::vector<VoiceCharacter> tmp(n);
	for (uint32_t i = 0; i < n && !r.bad(); ++i) {
		tmp[i].volume = int(r.u32());
		tmp[i].enabled = r.u8();
		tmp[i].flag = r.u8();
	}
	if (n == chars.size())
		for (uint32_t i = 0; i < n; ++i) {
			chars[i].volume = tmp[i].volume;
			chars[i].enabled = tmp[i].enabled;
			chars[i].flag = tmp[i].flag;
		}
	uint32_t stored = r.u32();
	if (!stored) return r.bad() ? 2 : 0;
	uint32_t orig = r.u32(), count = r.u32();
	std::vector<uint8_t> blob = r.bytes(stored);
	if (r.bad()) return 2;
	if (orig) blob = lzss_decompress(blob.data(), blob.size(), orig);
	ByteReader b(blob);
	auto &table = vm.init_table();
	for (uint32_t k = 0; k < count && !b.bad(); ++k) {
		std::string name = b.str(b.u8());
		while (!name.empty() && name.back() == '\0') name.pop_back();
		uint32_t na = b.u32();
		std::vector<uint8_t> a = b.bytes(b.u32());
		uint32_t nb = b.u32();
		std::vector<uint8_t> bb = b.bytes(b.u32());
		for (auto &e : table) {
			if (e.name != name) continue;
			if (na == e.count_a && a.size() == e.bits_a.size()) e.bits_a = a;
			if (nb == e.count_b && bb.size() == e.bits_b.size()) e.bits_b = bb;
		}
	}
	return 0;
}

// ---------------------------------------------------------------- natives

void register_save_natives(Engine &e) {
	auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
		e.vm.set_native(idx, [fn](Vm &v, uint32_t a) {
			Args args(v, a);
			return fn(args);
		}, name);
	};
	def(0x1a, "reset_vars", [&e](Args &a) {
		e.copy_vars(1, 0, a.i(0), a.i(1));
		return 0;
	});
	def(0x1c, "snapshot_vars", [&e](Args &a) {
		e.copy_vars(0, 2, a.i(0), a.i(1));
		return 0;
	});
	def(0x22, "gamedata_name", [&e](Args &a) {
		e.gamedata_name = a.s(0);
		return 0;
	});
	def(0x23, "write_global", [&e](Args &) {
		int r = e.write_gamedata();
		int r2 = e.write_textlog();
		return r ? r : r2;
	});
	def(0x24, "read_global", [&e](Args &) { return e.read_gamedata(); });
	def(0x1e9, "textlog_name", [&e](Args &a) {
		e.textlog_name = a.s(0);
		return 0;
	});
	def(0x1ea, "read_textlog", [&e](Args &) { return e.read_textlog(); });
	def(0x27, "save_info", [&e](Args &a) {
		if (!a.i(0)) return 0;
		e.save_code = a.i(0);
		e.save_title_ptr = a.u(1);
		e.save_preview_flag_ptr = a.u(2);
		e.live_save_vars.clear();
		return 1;
	});
	def(0x28, "save_live_var", [&e](Args &a) {
		uint32_t addr = a.u(0);
		const auto &gl = e.vm.globals();
		for (size_t i = 0; i < gl.size(); ++i)
			if (gl[i].addr[0] == addr) {
				e.live_save_vars.push_back(int(i));
				return 1;
			}
		return 0;
	});
	def(0x29, "thumbnail_source", [&e](Args &a) {
		e.thumb_source = a.u(0);
		e.thumb_name_ptr = a.u(1);
		return 1;
	});
	def(0x37, "thumbnail_flags", [&e](Args &a) {
		int sel = a.i(1);
		if (sel == 0 || sel == 1) e.thumb_flag_ptr[sel] = a.u(2);
		return 0;
	});
	def(0x2a, "save_game", [&e](Args &a) { return e.save_game(a.i(0), a.i(1)); });
	def(0x2b, "load_game", [&e](Args &a) { return e.load_game(a.i(0), a.i(1)); });
	def(0x2c, "save_description", [&e](Args &a) {
		if (a.null(0)) return 1;
		int bank = a.i(1), slot = a.i(2);
		// Quick load asks for slot -1 before showing its confirmation dialog.
		// Resolve the same newest file that load_game will read.
		slot = ring_slot(e, bank, slot, false);
		if (slot < 0) return -1;
		std::string path = e.save_path(save_file_name(bank, slot));
		std::vector<uint8_t> d;
		if (!read_save_file(path, d) || d.size() < kHeaderAt) return -1;
		std::string title(reinterpret_cast<const char *>(&d[kTitleAt]), strnlen(reinterpret_cast<const char *>(&d[kTitleAt]), 0x2c));
		struct stat st;
		stat(path.c_str(), &st);
		struct tm lt;
		localtime_r(&st.st_mtime, &lt);
		bool gbk = e.encoding == TextEncoding::Gbk;
		static const char *wd_sjis[] = {"\x93\xfa", "\x8c\x8e", "\x89\xce", "\x90\x85", "\x96\xd8", "\x8b\xe0", "\x93\x79"};
		static const char *wd_gbk[] = {"\xc8\xd5", "\xd2\xbb", "\xb6\xfe", "\xc8\xfd", "\xcb\xc4", "\xce\xe5", "\xc1\xf9"};
		char date[64];
		snprintf(date, sizeof date, "%04d/%02d/%02d(%s) %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
		         (gbk ? wd_gbk : wd_sjis)[lt.tm_wday], lt.tm_hour, lt.tm_min);
		const char *sep = a.i(3) == 0 ? " " : a.i(3) == 1 ? "\n" : "\r\n";
		// "『%s』" + sep + "【記録日時 %s】" (Chinese patch: 【存档时间 %s】)
		std::string open = gbk ? "\xa1\xba" : "\x81\x77", close = gbk ? "\xa1\xbb" : "\x81\x78";
		std::string label = gbk ? "\xa1\xbe\xb4\xe6\xb5\xb5\xca\xb1\xbc\xe4 " : "\x81\x79\x8b\x4c\x98\x5e\x93\xfa\x8e\x9e ";
		std::string end = gbk ? "\xa1\xbf" : "\x81\x7a";
		a.out_str(0, open + title + close + sep + label + date + end);
		return 0;
	});
	def(0x2d, "save_slot_info", [&e](Args &a) {
		int bank = a.i(0), slot = a.i(1);
		std::string path = e.save_path(save_file_name(bank, slot));
		std::vector<uint8_t> d;
		if (!read_save_file(path, d)) return -1;
		if (d.size() < kHeaderAt || memcmp(d.data(), "OT01", 4) != 0) return 2;
		if (!a.null(2)) {
			if (ImageRef img = e.image_of(a.u(2))) {
				for (size_t y = 0; y < kThumbH && int(y) < img->height(); ++y)
					for (size_t x = 0; x < kThumbW && int(x) < img->width(); ++x) {
						const uint8_t *s = &d[8 + (y * kThumbW + x) * 3];
						img->row32(int(y))[x] = uint32_t(s[0]) | (uint32_t(s[1]) << 8) | (uint32_t(s[2]) << 16);
					}
				gfx_touch();
			}
		}
		if (!a.null(3)) {
			std::string t(reinterpret_cast<const char *>(&d[kTitleAt]), strnlen(reinterpret_cast<const char *>(&d[kTitleAt]), 0x2c));
			a.out_str(3, t, 0x2c);
		}
		if (!a.null(4)) {
			std::string p(reinterpret_cast<const char *>(&d[kPreviewAt]), strnlen(reinterpret_cast<const char *>(&d[kPreviewAt]), 0x90));
			if (a.b(6) && e.text) p = e.text->reflow(p, 34, 4);
			a.out_str(4, p);
		}
		if (!a.null(5)) {
			struct stat st;
			stat(path.c_str(), &st);
			struct tm lt;
			localtime_r(&st.st_mtime, &lt);
			bool gbk = e.encoding == TextEncoding::Gbk;
			static const char *wd_sjis[] = {"\x93\xfa", "\x8c\x8e", "\x89\xce", "\x90\x85", "\x96\xd8", "\x8b\xe0", "\x93\x79"};
			static const char *wd_gbk[] = {"\xc8\xd5", "\xd2\xbb", "\xb6\xfe", "\xc8\xfd", "\xcb\xc4", "\xce\xe5", "\xc1\xf9"};
			char date[64];
			snprintf(date, sizeof date, "%04d/%02d/%02d(%s) %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
			         (gbk ? wd_gbk : wd_sjis)[lt.tm_wday], lt.tm_hour, lt.tm_min);
			a.out_str(5, date);
		}
		return 0;
	});
	def(0x2e, "save_set_preview", [&e](Args &a) {
		std::string path = e.save_path(save_file_name(a.i(0), a.i(1)));
		std::vector<uint8_t> d;
		if (!read_save_file(path, d)) return -1;
		if (d.size() < kHeaderAt) return 1;
		std::string t = a.s(2);
		if (t.size() > 140) t.resize(140);
		memset(&d[kPreviewAt], 0, 0x90);
		memcpy(&d[kPreviewAt], t.data(), t.size());
		return write_file(path, d) ? 0 : 1;
	});
	def(0x2f, "save_copy", [&e](Args &a) {
		std::vector<uint8_t> d;
		if (!read_save_file(e.save_path(save_file_name(a.i(0), a.i(1))), d)) return 1;
		return write_file(e.save_path(save_file_name(a.i(2), a.i(3))), d) ? 0 : 3;
	});
	def(0x30, "save_delete", [&e](Args &a) {
		std::string p = e.save_path(save_file_name(a.i(0), a.i(1)));
		struct stat st;
		if (stat(p.c_str(), &st) != 0) return 1;
		return remove(p.c_str()) == 0 ? 0 : 2;
	});
	def(0x31, "snapshot_take", [&e](Args &) {
		e.has_mem_snapshot = false;  // never nest the previous snapshot
		e.mem_snapshot = e.state_blob(!e.opt8(0xed9));
		e.has_mem_snapshot = true;
		return 0;
	});
	def(0x32, "snapshot_restore", [&e](Args &) {
		if (!e.has_mem_snapshot) return -1;
		std::vector<uint8_t> s = std::move(e.mem_snapshot);
		e.has_mem_snapshot = false;
		return e.restore_state_blob(s);
	});
	def(0x33, "snapshot_exists", [&e](Args &) { return e.has_mem_snapshot ? 1 : 0; });
	def(0x34, "snapshot_free", [&e](Args &) {
		e.mem_snapshot.clear();
		e.has_mem_snapshot = false;
		return 0;
	});
}

}  // namespace ore
