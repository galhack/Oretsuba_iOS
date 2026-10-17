// Reads native-call arguments from VM memory.
#pragma once

#include <cstdint>
#include <string>

#include "vm/vm.h"

namespace ore {

class Args {
public:
	Args(Vm &vm, uint32_t base) : vm_(vm), base_(base) {}

	uint32_t u(int i) const { return vm_.mem().rd32(base_ + uint32_t(i) * 4); }
	int32_t i(int i) const { return int32_t(u(i)); }
	bool b(int i) const { return u(i) != 0; }
	// Address of argument i itself (for varargs such as wsprintf).
	uint32_t addr(int i) const { return base_ + uint32_t(i) * 4; }
	// C string argument (empty for NULL).
	const char *s(int i) const {
		uint32_t p = u(i);
		return p ? vm_.mem().cstr(p) : "";
	}
	bool null(int i) const { return u(i) == 0; }
	std::string str(int i) const { return s(i); }

	// Writes through a pointer argument (int* / handle*).
	void out(int i, int32_t v) const {
		uint32_t p = u(i);
		if (p) vm_.mem().wr32(p, uint32_t(v));
	}
	// Copies a string into a char* argument (with a byte limit, 0 = none).
	void out_str(int i, const std::string &v, size_t limit = 0) const {
		uint32_t p = u(i);
		if (!p) return;
		size_t n = v.size();
		if (limit && n + 1 > limit) n = limit - 1;
		uint8_t *d = vm_.mem().host(p, n + 1);
		memcpy(d, v.data(), n);
		d[n] = 0;
	}

	Vm &vm() const { return vm_; }

private:
	Vm &vm_;
	uint32_t base_;
};

}  // namespace ore
