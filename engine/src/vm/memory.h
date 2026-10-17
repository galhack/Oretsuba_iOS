// 32-bit virtual address space for the SOB VM.
//
// SOB bytecode is self-modifying and stores absolute 32-bit addresses, so all
// script-visible memory (code blobs, globals, the VM stack, strings handed to
// scripts) lives in one arena addressed as kBase + offset.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ore {

class VmMemory {
public:
	static constexpr uint32_t kBase = 0x10000000;

	explicit VmMemory(size_t capacity = 96u << 20);
	~VmMemory();
	VmMemory(const VmMemory &) = delete;
	VmMemory &operator=(const VmMemory &) = delete;

	// Allocates zeroed memory; returns its VM address (never 0).
	uint32_t alloc(size_t n, const char *tag = "");
	void free(uint32_t addr);
	size_t block_size(uint32_t addr) const;

	bool valid(uint32_t addr, size_t n) const {
		return addr >= kBase && uint64_t(addr - kBase) + n <= top_;
	}
	uint8_t *host(uint32_t addr, size_t n = 1);  // aborts the VM on bad access
	const uint8_t *host(uint32_t addr, size_t n = 1) const;

	uint32_t rd32(uint32_t addr) const {
		uint32_t v;
		memcpy(&v, host(addr, 4), 4);
		return v;
	}
	void wr32(uint32_t addr, uint32_t v) { memcpy(host(addr, 4), &v, 4); }

	// NUL-terminated string at addr (bounded by the arena).
	const char *cstr(uint32_t addr) const;
	uint32_t alloc_string(const std::string &s, const char *tag = "str");

	// Set when an access fell outside the arena; the VM stops on it.
	bool faulted() const { return fault_; }
	uint32_t fault_addr() const { return fault_addr_; }
	void clear_fault() { fault_ = false; }

private:
	uint8_t *mem_ = nullptr;
	size_t capacity_ = 0;
	size_t top_ = 0;  // high-water mark of used address space
	std::map<uint32_t, size_t> used_;   // offset -> size
	std::map<uint32_t, size_t> free_;   // offset -> size
	mutable bool fault_ = false;
	mutable uint32_t fault_addr_ = 0;
	mutable uint8_t scratch_[16] = {};
};

}  // namespace ore
