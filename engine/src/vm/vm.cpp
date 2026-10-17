#include "vm/vm.h"

#include <cstdio>

#include "util/log.h"
#include "util/text.h"

namespace ore {
namespace {

constexpr int kNativeTableSize = 798;
constexpr uint32_t kStackWords = 0x4000;  // FUN_004226f0(engine, 0x4000)
constexpr uint64_t kFrameBudget = 200'000'000;

uint32_t rd32(const uint8_t *p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}
uint16_t rd16(const uint8_t *p) {
	return uint16_t(p[0] | (p[1] << 8));
}

}  // namespace

Vm::Vm(FileReader reader) : reader_(std::move(reader)) {
	natives_.resize(kNativeTableSize);
	native_names_.resize(kNativeTableSize);
	native_warned_.assign(kNativeTableSize, false);
	stack_size_ = kStackWords * 4;
	stack_ = mem_.alloc(stack_size_, "stack");
	sp_ = stack_;
	exported_ = mem_.alloc(16, "exported");
}

void Vm::set_native(int index, NativeFn fn, const char *name) {
	if (index < 0 || index >= kNativeTableSize) return;
	natives_[index] = std::move(fn);
	native_names_[index] = name ? name : "";
}

void Vm::fail(const std::string &msg) {
	if (error_.empty()) error_ = msg;
	log_error("vm: %s (pc=%s)", msg.c_str(), where(pc_).c_str());
}

std::string Vm::where(uint32_t addr) const {
	char buf[96];
	auto inside = [&](const ScriptBlob &b) { return addr >= b.base && addr < b.base + b.size; };
	for (auto it = scripts_.rbegin(); it != scripts_.rend(); ++it) {
		if (inside(*it)) {
			snprintf(buf, sizeof buf, "%s+%x", it->name.c_str(), addr - it->base);
			return buf;
		}
	}
	if (func_lib_.base && inside(func_lib_)) {
		snprintf(buf, sizeof buf, "FUNC_LIB+%x", addr - func_lib_.base);
		return buf;
	}
	snprintf(buf, sizeof buf, "%08x", addr);
	return buf;
}

uint32_t Vm::global_addr(int index) const {
	if (index < 0 || index >= int(globals_.size())) return 0;
	return globals_[index].addr[0];
}

uint32_t Vm::exported_addr(int index) const {
	return index >= 0 && index < 4 ? exported_ + uint32_t(index) * 4 : 0;
}

bool Vm::load_init_table(const std::vector<uint8_t> &d) {
	// u16 count; per entry: u8 len, name[len], u16 a, u16 b  (FUN_00424440)
	if (d.size() < 2) return false;
	size_t p = 0;
	unsigned count = rd16(&d[p]);
	p += 2;
	init_table_.clear();
	for (unsigned i = 0; i < count; ++i) {
		if (p + 1 > d.size()) return false;
		unsigned len = d[p++];
		if (p + len + 4 > d.size()) return false;
		ScriptInitEntry e;
		e.name = to_upper_ascii(std::string(reinterpret_cast<const char *>(&d[p]), len));
		while (!e.name.empty() && e.name.back() == '\0') e.name.pop_back();
		p += len;
		e.count_a = rd16(&d[p]);
		e.count_b = rd16(&d[p + 2]);
		p += 4;
		e.bits_a.assign((e.count_a + 7) / 8, 0);
		e.bits_b.assign((e.count_b + 7) / 8, 0);
		init_table_.push_back(std::move(e));
	}
	return true;
}

bool Vm::load_user_vars(const std::vector<uint8_t> &d, std::string *error) {
	// See ore_tuba.c around FUN at 0x424440 (USER_VAR.SOB = SOB1).
	if (d.size() < 12 || rd32(&d[0]) != 0x31424f53) {
		if (error) *error = "USER_VAR.SOB: bad magic";
		return false;
	}
	user_size_ = rd32(&d[4]);
	size_t p = 8;
	if (p + user_size_ + 4 > d.size()) {
		if (error) *error = "USER_VAR.SOB: truncated";
		return false;
	}
	for (int c = 0; c < 3; ++c) {
		user_area_[c] = mem_.alloc(user_size_, "user_var");
		memcpy(mem_.host(user_area_[c], user_size_), &d[p], user_size_);
	}
	p += user_size_;
	uint32_t count = rd32(&d[p]);
	p += 4;
	std::vector<uint32_t> indirect[3];
	globals_.clear();
	for (uint32_t i = 0; i < count; ++i) {
		if (p + 10 > d.size()) {
			if (error) *error = "USER_VAR.SOB: truncated var table";
			return false;
		}
		Global g;
		g.type = d[p];
		g.flags = d[p + 1];
		uint32_t off = rd32(&d[p + 2]);
		g.extra = rd32(&d[p + 6]);
		p += 10;
		for (int c = 0; c < 3; ++c) {
			if (g.type == 1) {
				indirect[c].push_back(user_area_[c] + off);
				g.addr[c] = 0;  // assigned below: address of its pointer cell
			} else {
				g.addr[c] = user_area_[c] + off;
			}
		}
		globals_.push_back(g);
	}
	// Indirect variables: the global refers to a cell holding the pointer.
	if (!indirect[0].empty()) {
		for (int c = 0; c < 3; ++c) {
			indirect_cells_[c] = mem_.alloc(indirect[c].size() * 4, "user_var_cells");
			for (size_t k = 0; k < indirect[c].size(); ++k)
				mem_.wr32(indirect_cells_[c] + uint32_t(k) * 4, indirect[c][k]);
		}
		size_t k = 0;
		for (Global &g : globals_) {
			if (g.addr[0] != 0) continue;
			for (int c = 0; c < 3; ++c) g.addr[c] = indirect_cells_[c] + uint32_t(k) * 4;
			++k;
		}
	}
	// Internal pointer fixups: *(area+off) = area+val.
	if (p + 4 > d.size()) return true;
	uint32_t nfix = rd32(&d[p]);
	p += 4;
	for (uint32_t i = 0; i < nfix && p + 8 <= d.size(); ++i, p += 8) {
		uint32_t off = rd32(&d[p]), val = rd32(&d[p + 4]);
		for (int c = 0; c < 3; ++c) mem_.wr32(user_area_[c] + off, user_area_[c] + val);
	}
	return true;
}

bool Vm::load_blob(const std::vector<uint8_t> &d, size_t at, const std::string &name, ScriptBlob &out,
                   std::string *error) {
	// at -> u32 hsize; reloc tables; u32 code_size; u32 blob_size; blob
	auto bad = [&](const char *why) {
		if (error) *error = name + ": " + why;
		return false;
	};
	if (at + 4 > d.size()) return bad("truncated header");
	uint32_t hsize = rd32(&d[at]);
	size_t tables = at + 4, end = tables + hsize;
	if (end + 8 > d.size()) return bad("truncated header");
	uint32_t code_size = rd32(&d[end]);
	uint32_t blob_size = rd32(&d[end + 4]);
	if (end + 8 + blob_size > d.size()) return bad("truncated blob");
	out.name = name;
	out.size = blob_size;
	out.code_size = code_size;
	out.base = mem_.alloc(blob_size + 16, "script");
	memcpy(mem_.host(out.base, blob_size), &d[end + 8], blob_size);
	size_t p = tables;
	bool have_base = false;
	for (int t = 0; t < 5; ++t) {
		if (p + 4 > end) return bad("bad relocation tables");
		uint32_t n = rd32(&d[p]);
		p += 4;
		if (p + size_t(n) * 8 > end) return bad("bad relocation tables");
		for (uint32_t i = 0; i < n; ++i, p += 8) {
			uint32_t off = rd32(&d[p]), val = rd32(&d[p + 4]);
			if (off + 4 > blob_size) return bad("relocation outside blob");
			uint32_t at_addr = out.base + off;
			uint32_t v = 0;
			switch (t) {
			case 0:
				if (val >= globals_.size()) return bad("bad global index");
				v = globals_[val].addr[0];
				break;
			case 1:
				v = 0x80000000u | val;  // native table index tag
				break;
			case 2:
				v = exported_addr(int(val));
				break;
			case 3:
				if (!func_lib_.base) return bad("FUNC_LIB reference before FUNC_LIB load");
				v = func_lib_.base + val;
				break;
			case 4:
				if (!have_base) {
					out.compile_base = mem_.rd32(at_addr) - val;
					have_base = true;
				}
				v = out.base + val;
				break;
			}
			mem_.wr32(at_addr, v);
		}
	}
	if (p + 4 > end || rd32(&d[p]) != 0) return bad("unexpected extra relocation table");
	return true;
}

bool Vm::init(std::string *error) {
	std::vector<uint8_t> d;
	if (reader_("SCRIPT\\SOB_INIT.DAT", d)) load_init_table(d);
	if (!reader_("SCRIPT\\USER_VAR.SOB", d)) {
		if (error) *error = "missing SCRIPT\\USER_VAR.SOB";
		return false;
	}
	if (!load_user_vars(d, error)) return false;
	if (!reader_("SCRIPT\\FUNC_LIB.SOB", d) || d.size() < 8 || rd32(&d[0]) != 0x32424f53) {
		if (error) *error = "missing or bad SCRIPT\\FUNC_LIB.SOB";
		return false;
	}
	// FUNC_LIB references itself through table 3, so its base must be known
	// before relocation: allocate, then relocate in place.
	ScriptBlob lib;
	func_lib_.base = 1;  // placeholder so table-3 entries are accepted
	{
		uint32_t hsize = rd32(&d[4]);
		size_t end = 8 + hsize;
		uint32_t blob_size = end + 8 <= d.size() ? rd32(&d[end + 4]) : 0;
		func_lib_.base = mem_.alloc(blob_size + 16, "func_lib_probe");
		mem_.free(func_lib_.base);  // the next allocation of this size lands here
	}
	if (!load_blob(d, 4, "FUNC_LIB", lib, error)) return false;
	if (lib.base != func_lib_.base) {
		if (error) *error = "FUNC_LIB base mismatch";
		return false;
	}
	func_lib_ = lib;
	return true;
}

bool Vm::load_script(const std::string &raw_name, std::string *error) {
	std::string name = to_upper_ascii(raw_name);
	std::vector<uint8_t> d;
	if (!reader_("SCRIPT\\" + name + ".SOB", d)) {
		if (error) *error = "missing script " + name;
		return false;
	}
	if (d.size() < 8 || rd32(&d[0]) != 0x30424f53) {
		if (error) *error = "bad script " + name;
		return false;
	}
	ScriptBlob b;
	if (!load_blob(d, 4, name, b, error)) return false;
	for (size_t i = 0; i < init_table_.size(); ++i) {
		if (init_table_[i].name == name) {
			b.init_index = int(i);
			break;
		}
	}
	scripts_.push_back(b);
	pc_ = b.base;
	push(0);  // sentinel return address (FUN_004335d0)
	return true;
}

void Vm::unload_top() {
	if (scripts_.empty()) return;
	mem_.free(scripts_.back().base);
	scripts_.pop_back();
}

bool Vm::start(const std::string &name, std::string *error) {
	while (!scripts_.empty()) unload_top();
	sp_ = stack_;
	call_depth_ = 0;
	mode_ = 0;
	error_.clear();
	return load_script(name, error);
}

uint32_t Vm::translate_switch_target(uint32_t target) const {
	if (mem_.valid(target, 4)) return target;
	// Unrelocated table entry: convert from the compiler's address space.
	auto fix = [&](const ScriptBlob &b) -> uint32_t {
		uint32_t off = target - b.compile_base;
		return off < b.size ? b.base + off : 0;
	};
	for (auto it = scripts_.rbegin(); it != scripts_.rend(); ++it) {
		if (pc_ >= it->base && pc_ < it->base + it->size) return fix(*it);
	}
	return fix(func_lib_);
}

int Vm::exec(uint64_t budget) {
	VmMemory &m = mem_;
	auto rd = [&](uint32_t a) { return m.rd32(a); };
	auto wr = [&](uint32_t a, uint32_t v) { m.wr32(a, v); };
	for (uint64_t n = 0; n < budget; ++n) {
		if (m.faulted()) {
			char buf[64];
			snprintf(buf, sizeof buf, "memory fault at %08x", m.fault_addr());
			fail(buf);
			return -1;
		}
		uint32_t op = rd(pc_);
		uint32_t a = pc_ + 4;  // first operand
		++steps_;
		if (trace) log_info("vm %s op %u", where(pc_).c_str(), op);
		switch (op) {
		case 0:  // end (yield)
			pc_ = a;
			return 1;
		case 1: {  // ret
			uint32_t r = pop();
			if (!r) return 0;
			pc_ = r;
			break;
		}
		case 2: {  // retv
			uint32_t r = pop();
			if (!r) return 0;
			ret_ = rd(a);
			pc_ = r;
			break;
		}
		case 3:
			wr(rd(a), ret_);
			pc_ = a + 4;
			break;
		case 4:
			push(a + 4);
			pc_ = rd(a);
			break;
		case 5: {
			uint32_t f = rd(a), args = rd(a + 4);
			pc_ = a + 8;  // the native may inspect/redirect pc
			int32_t result = 0;
			uint32_t idx = f & 0x7FFFFFFF;
			if (!(f & 0x80000000u) || idx >= natives_.size()) {
				fail("call to non-native address");
				return -1;
			}
			if (natives_[idx]) {
				retry_ = false;
				result = natives_[idx](*this, args);
				if (retry_) {
					retry_ = false;
					pc_ = a - 4;
					return 1;
				}
			} else if (!native_warned_[idx]) {
				native_warned_[idx] = true;
				log_warn("vm: unimplemented native N_%x at %s", idx, where(a - 4).c_str());
			}
			if (idx < trace_natives.size() && trace_natives[idx]) {
				if (m.valid(args, 16))
					log_info("N_%x(%d, %d, %d, %d) = %d at %s", idx, int32_t(rd(args)), int32_t(rd(args + 4)),
					         int32_t(rd(args + 8)), int32_t(rd(args + 12)), result, where(a - 4).c_str());
				else
					log_info("N_%x(arguments unavailable) = %d at %s", idx, result, where(a - 4).c_str());
			}
			wr(rd(pc_), uint32_t(result));
			pc_ += 4;
			break;
		}
		case 6:
			pc_ = rd(a);
			break;
		case 7: {
			uint32_t i = rd(a);
			uint32_t t = translate_switch_target(rd(a + 4 + i * 4));
			if (!t) {
				fail("bad switch target");
				return -1;
			}
			pc_ = t;
			break;
		}
		case 8:
			push(rd(a));
			pc_ = a + 4;
			break;
		case 9:
			push(rd(rd(a)));
			pc_ = a + 4;
			break;
		case 10:
			wr(rd(a), pop());
			pc_ = a + 4;
			break;
		case 11:
			sp_ += rd(a);
			pc_ = a + 4;
			break;
		case 12:
			sp_ -= rd(a);
			pc_ = a + 4;
			break;
		case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21:
		case 24: case 25: case 41: case 42: case 43: case 44: case 45: case 46: {
			int32_t x = int32_t(rd(a)), y = int32_t(rd(a + 4));
			uint32_t d = rd(a + 8);
			int32_t r = 0;
			switch (op) {
			case 13: r = int32_t(uint32_t(x) + uint32_t(y)); break;
			case 14: r = int32_t(uint32_t(x) - uint32_t(y)); break;
			case 15: r = int32_t(uint32_t(x) * uint32_t(y)); break;
			case 16: r = (y == 0 || (x == INT32_MIN && y == -1)) ? 0 : x / y; break;
			case 17: r = (y == 0 || (x == INT32_MIN && y == -1)) ? 0 : x % y; break;
			case 18:
				r = (y == 0 || (x == INT32_MIN && y == -1)) ? 0 : x % y;
				if (r < 0) r += y;
				break;
			case 19: r = x & y; break;
			case 20: r = x | y; break;
			case 21: r = x ^ y; break;
			case 24: r = x >> (y & 31); break;
			case 25: r = int32_t(uint32_t(x) << (y & 31)); break;
			case 41: r = x < y; break;
			case 42: r = x <= y; break;
			case 43: r = x > y; break;
			case 44: r = x >= y; break;
			case 45: r = x == y; break;
			case 46: r = x != y; break;
			}
			if (y == 0 && (op == 16 || op == 17 || op == 18)) log_warn("vm: division by zero at %s", where(pc_).c_str());
			wr(d, uint32_t(r));
			pc_ = a + 12;
			break;
		}
		case 22:
			wr(rd(a + 4), ~rd(a));
			pc_ = a + 8;
			break;
		case 23:
			wr(rd(a + 4), rd(a) == 0);
			pc_ = a + 8;
			break;
		case 26: {
			uint32_t p = rd(a);
			wr(p, rd(p) + 1);
			pc_ = a + 4;
			break;
		}
		case 27: {
			uint32_t p = rd(a);
			wr(p, rd(p) - 1);
			pc_ = a + 4;
			break;
		}
		case 28: {
			uint32_t p = sp_ - rd(a);
			wr(p, rd(p) + 1);
			pc_ = a + 4;
			break;
		}
		case 29: {
			uint32_t p = sp_ - rd(a);
			wr(p, rd(p) - 1);
			pc_ = a + 4;
			break;
		}
		case 30:
			pc_ = rd(a) == 0 ? rd(a + 4) : a + 8;
			break;
		case 31:
			wr(rd(a + 4), rd(a));
			pc_ = a + 8;
			break;
		case 32:
			wr(rd(a + 4), rd(rd(a)));
			pc_ = a + 8;
			break;
		case 33:
			wr(rd(rd(a + 4)), rd(a));
			pc_ = a + 8;
			break;
		case 34:
			wr(sp_ - rd(a + 4), rd(a));
			pc_ = a + 8;
			break;
		case 35:
			wr(rd(a + 4), rd(sp_ - rd(a)));
			pc_ = a + 8;
			break;
		case 36:
			wr(sp_ - rd(a + 4), rd(rd(a)));
			pc_ = a + 8;
			break;
		case 37:
			wr(sp_ - rd(a + 4), rd(sp_ - rd(a)));
			pc_ = a + 8;
			break;
		case 38:
			wr(rd(a + 4), rd(rd(sp_ - rd(a))));
			pc_ = a + 8;
			break;
		case 39:
			wr(rd(a + 4), rd(rd(rd(a))));
			pc_ = a + 8;
			break;
		case 40:
			wr(rd(a + 4), sp_ - rd(a));
			pc_ = a + 8;
			break;
		default: {
			char buf[48];
			snprintf(buf, sizeof buf, "bad opcode %u", op);
			fail(buf);
			return -1;
		}
		}
		if (sp_ < stack_ || sp_ > stack_ + stack_size_) {
			fail("stack overflow/underflow");
			return -1;
		}
	}
	fail("instruction budget exhausted (infinite loop?)");
	return -1;
}

Vm::RunResult Vm::run_frame() {
	if (!error_.empty() || scripts_.empty()) return RunResult::Error;
	// Mirrors the scheduler FUN_004252d0.
	for (;;) {
		int r = exec(kFrameBudget);
		if (r < 0) return RunResult::Error;
		int mode = mode_;
		switch (mode) {
		case 0:
			if (r) return RunResult::Yield;
			if (call_depth_ == 0) return RunResult::Finished;
			--call_depth_;
			unload_top();
			pc_ = pop();
			break;
		case 1: {
			if (r) {
				fail("script yielded while a goto was pending");
				return RunResult::Error;
			}
			unload_top();
			mode_ = 0;
			std::string err;
			if (!load_script(pending_, &err)) {
				fail(err);
				return RunResult::Error;
			}
			break;
		}
		case 2: {
			if (!r) {
				fail("script stopped while a call was pending");
				return RunResult::Error;
			}
			push(pc_);
			mode_ = 0;
			std::string err;
			if (!load_script(pending_, &err)) {
				pc_ = pop();
				fail(err);
				return RunResult::Error;
			}
			++call_depth_;
			break;
		}
		case 3:
			mode_ = 0;
			return RunResult::Finished;
		default:
			mode_ = 0;
			break;
		}
	}
}

}  // namespace ore
