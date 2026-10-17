// Boots the VM with only the script-control natives and reports how far the
// scripts get and which natives they call.
#include <cstdio>
#include <map>
#include <string>

#include "res/vfs.h"
#include "util/log.h"
#include "vm/vm.h"

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: vm_smoke DATA_DIR START_SCRIPT [frames]\n");
		return 2;
	}
	ore::Vfs vfs;
	std::string err;
	if (!vfs.mount_archives(argv[1], &err)) {
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	ore::Vm vm([&](const std::string &p, std::vector<uint8_t> &out) { return vfs.read(p, out); });
	std::map<int, int> calls;
	for (int i = 3; i < vm.native_count(); ++i) {
		vm.set_native(i, [i, &calls](ore::Vm &, uint32_t) -> int32_t {
			++calls[i];
			return 0;
		});
	}
	vm.set_native(6, [](ore::Vm &, uint32_t) { return 1; });
	vm.set_native(0, [](ore::Vm &v, uint32_t a) {
		v.request_call(v.mem().cstr(v.mem().rd32(a)));
		return 0;
	});
	vm.set_native(1, [](ore::Vm &v, uint32_t a) {
		v.request_goto(v.mem().cstr(v.mem().rd32(a)));
		return 0;
	});
	vm.set_native(2, [](ore::Vm &v, uint32_t) {
		v.request_stop();
		return 0;
	});
	if (!vm.init(&err) || !vm.start(argv[2], &err)) {
		fprintf(stderr, "init: %s\n", err.c_str());
		return 1;
	}
	int frames = argc > 3 ? atoi(argv[3]) : 100;
	std::string last;
	for (int f = 0; f < frames; ++f) {
		auto r = vm.run_frame();
		const ore::ScriptBlob *s = vm.current_script();
		std::string now = s ? s->name : "-";
		if (now != last) printf("frame %d: script %s depth=%zu\n", f, now.c_str(), vm.script_stack().size());
		last = now;
		if (r != ore::Vm::RunResult::Yield) {
			printf("frame %d: result %d error='%s' at %s\n", f, int(r), vm.error().c_str(), vm.where(vm.pc()).c_str());
			break;
		}
	}
	printf("steps=%llu natives:", (unsigned long long)vm.steps());
	for (auto &kv : calls) printf(" %x:%d", kv.first, kv.second);
	printf("\n");
	return 0;
}
