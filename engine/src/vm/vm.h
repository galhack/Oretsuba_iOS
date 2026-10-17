// Lucifen SOB script virtual machine (see docs/re/00_vm.md).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vm/memory.h"

namespace ore {

class Vm;

// args = VM address of the first argument word.
using NativeFn = std::function<int32_t(Vm &vm, uint32_t args)>;

struct ScriptBlob {
	std::string name;        // upper-case script name, e.g. "TITLE"
	uint32_t base = 0;       // VM address of the relocated blob
	uint32_t size = 0;
	uint32_t code_size = 0;
	uint32_t compile_base = 0;  // address the compiler assumed for offset 0
	int init_index = -1;        // index into the sob_init table (read flags)
};

// One sob_init.dat entry: per-script line counts used for read-text flags.
struct ScriptInitEntry {
	std::string name;
	uint16_t count_a = 0;
	uint16_t count_b = 0;
	std::vector<uint8_t> bits_a;  // one bit per line
	std::vector<uint8_t> bits_b;
};

class Vm {
public:
	enum class RunResult { Yield, Finished, Error };
	using FileReader = std::function<bool(const std::string &path, std::vector<uint8_t> &out)>;

	explicit Vm(FileReader reader);

	VmMemory &mem() { return mem_; }

	// Natives: table index -> handler. Unregistered natives log once and return 0.
	void set_native(int index, NativeFn fn, const char *name = nullptr);
	int native_count() const { return int(natives_.size()); }
	NativeFn native(int index) const { return index >= 0 && index < int(natives_.size()) ? natives_[size_t(index)] : NativeFn(); }

	// Boot: loads SOB_INIT.DAT, USER_VAR.SOB and FUNC_LIB.SOB.
	bool init(std::string *error);
	// Starts a top-level script (replaces any running script stack).
	bool start(const std::string &name, std::string *error);

	// Runs until the script yields (one frame) or stops.
	RunResult run_frame();

	// Script-transition requests (natives N_0/N_1/N_2).
	void request_call(const std::string &name) { mode_ = 2, pending_ = name; }
	void request_goto(const std::string &name) { mode_ = 1, pending_ = name; }
	void request_stop() { mode_ = 3; }
	// Called by a native that cannot complete yet (e.g. waiting for a modal
	// dialog): the VM yields and re-executes the same native next frame.
	void retry_native() { retry_ = true; }

	// Globals.
	uint32_t global_addr(int index) const;
	int global_count() const { return int(globals_.size()); }
	// The three copies of the user variable area (0 = live).
	uint32_t user_area(int copy) const { return user_area_[copy]; }
	uint32_t user_area_size() const { return user_size_; }
	uint32_t exported_addr(int index) const;  // T[index]

	const std::vector<ScriptBlob> &script_stack() const { return scripts_; }
	const ScriptBlob *current_script() const { return scripts_.empty() ? nullptr : &scripts_.back(); }
	std::vector<ScriptInitEntry> &init_table() { return init_table_; }

	uint32_t pc() const { return pc_; }
	uint32_t sp() const { return sp_; }
	const std::string &error() const { return error_; }
	uint64_t steps() const { return steps_; }

	// Diagnostics: describe an address as "SCRIPT+off".
	std::string where(uint32_t addr) const;
	bool trace = false;
	// Natives whose calls are logged with their first arguments and result.
	std::vector<bool> trace_natives = std::vector<bool>(1024, false);

	struct Global {
		uint32_t addr[3];
		uint8_t type = 0;   // 0 value, 1 string buffer (addr = pointer cell), 2 scratch
		uint8_t flags = 0;  // scope: 0 volatile, 1 per-save, 2 global
		uint32_t extra = 0; // size in bytes
	};
	const std::vector<Global> &globals() const { return globals_; }
	// Address of a global's data in copy c (resolves string-buffer cells).
	uint32_t global_data(int index, int c) {
		const Global &g = globals_[size_t(index)];
		return g.type == 1 ? mem_.rd32(g.addr[c]) : g.addr[c];
	}

private:

	bool load_blob(const std::vector<uint8_t> &data, size_t header_at, const std::string &name,
	               ScriptBlob &out, std::string *error);
	bool load_script(const std::string &name, std::string *error);
	void unload_top();
	bool load_user_vars(const std::vector<uint8_t> &data, std::string *error);
	bool load_init_table(const std::vector<uint8_t> &data);
	int exec(uint64_t budget);  // 1 = yield, 0 = stopped, -1 = error
	uint32_t translate_switch_target(uint32_t target) const;
	void push(uint32_t v) {
		mem_.wr32(sp_, v);
		sp_ += 4;
	}
	uint32_t pop() {
		sp_ -= 4;
		return mem_.rd32(sp_);
	}
	void fail(const std::string &msg);

	FileReader reader_;
	VmMemory mem_;
	std::vector<NativeFn> natives_;
	std::vector<std::string> native_names_;
	std::vector<bool> native_warned_;
	std::vector<Global> globals_;
	uint32_t user_area_[3] = {0, 0, 0};
	uint32_t user_size_ = 0;
	uint32_t indirect_cells_[3] = {0, 0, 0};
	uint32_t exported_ = 0;  // T[] storage (3 words)
	std::vector<ScriptInitEntry> init_table_;
	ScriptBlob func_lib_;
	std::vector<ScriptBlob> scripts_;  // nested script stack, back = current

	uint32_t stack_ = 0, stack_size_ = 0;
	uint32_t pc_ = 0, sp_ = 0, ret_ = 0;
	int mode_ = 0;
	bool retry_ = false;
	std::string pending_;
	int call_depth_ = 0;
	std::string error_;
	uint64_t steps_ = 0;
};

}  // namespace ore
