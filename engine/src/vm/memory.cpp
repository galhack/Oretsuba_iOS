#include "vm/memory.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>

namespace ore {

VmMemory::VmMemory(size_t capacity) : capacity_(capacity) {
	// Reserve lazily-committed pages; untouched pages cost nothing.
	void *p = mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "VmMemory: mmap of %zu bytes failed\n", capacity_);
		abort();
	}
	mem_ = static_cast<uint8_t *>(p);
	top_ = 16;  // keep kBase+0 unused so no allocation has address kBase
}

VmMemory::~VmMemory() {
	munmap(mem_, capacity_);
}

uint32_t VmMemory::alloc(size_t n, const char *tag) {
	n = (n + 15) & ~size_t(15);
	if (n == 0) n = 16;
	for (auto it = free_.begin(); it != free_.end(); ++it) {
		if (it->second < n) continue;
		uint32_t off = it->first;
		size_t rest = it->second - n;
		free_.erase(it);
		if (rest) free_[off + uint32_t(n)] = rest;
		used_[off] = n;
		memset(mem_ + off, 0, n);
		return kBase + off;
	}
	if (top_ + n > capacity_) {
		fprintf(stderr, "VmMemory: out of arena memory (%zu bytes, tag %s)\n", n, tag);
		abort();
	}
	uint32_t off = uint32_t(top_);
	top_ += n;
	used_[off] = n;
	memset(mem_ + off, 0, n);
	return kBase + off;
}

void VmMemory::free(uint32_t addr) {
	if (!addr) return;
	auto it = used_.find(addr - kBase);
	if (it == used_.end()) {
		fprintf(stderr, "VmMemory: free of unknown block %08x\n", addr);
		return;
	}
	uint32_t off = it->first;
	size_t n = it->second;
	used_.erase(it);
	// Coalesce with neighbours.
	auto next = free_.find(off + uint32_t(n));
	if (next != free_.end()) {
		n += next->second;
		free_.erase(next);
	}
	auto prev = free_.lower_bound(off);
	if (prev != free_.begin()) {
		--prev;
		if (prev->first + prev->second == off) {
			prev->second += n;
			return;
		}
	}
	free_[off] = n;
}

size_t VmMemory::block_size(uint32_t addr) const {
	auto it = used_.find(addr - kBase);
	return it == used_.end() ? 0 : it->second;
}

uint8_t *VmMemory::host(uint32_t addr, size_t n) {
	if (!valid(addr, n)) {
		if (!fault_) {
			fault_ = true;
			fault_addr_ = addr;
		}
		return scratch_;
	}
	return mem_ + (addr - kBase);
}

const uint8_t *VmMemory::host(uint32_t addr, size_t n) const {
	return const_cast<VmMemory *>(this)->host(addr, n);
}

const char *VmMemory::cstr(uint32_t addr) const {
	if (!valid(addr, 1)) {
		fault_ = true;
		fault_addr_ = addr;
		return "";
	}
	const char *s = reinterpret_cast<const char *>(mem_ + (addr - kBase));
	if (!memchr(s, 0, top_ - (addr - kBase))) return "";
	return s;
}

uint32_t VmMemory::alloc_string(const std::string &s, const char *tag) {
	uint32_t a = alloc(s.size() + 1, tag);
	memcpy(host(a, s.size() + 1), s.c_str(), s.size() + 1);
	return a;
}

}  // namespace ore
