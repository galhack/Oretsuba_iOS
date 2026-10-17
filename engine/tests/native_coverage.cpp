// All original/overlay script native references must have engine handlers.
// This checks registration coverage, not every handler's runtime behaviour.
#include <cstdio>
#include <filesystem>
#include <set>
#include <unistd.h>

#include "core/engine.h"
#include "platform/apple/apple_platform.h"
#include "util/bytes.h"

int main(int argc, char **argv) {
	if (argc < 2 || argc > 3) { fprintf(stderr, "usage: native_coverage DATA [PATCH]\n"); return 2; }
	char saves[] = "/tmp/ore-native-coverage-XXXXXX";
	if (!mkdtemp(saves)) return 2;
	bool ok = true;
	{
		ore::ApplePlatform platform(saves);
		ore::EngineConfig config;
		config.data_dir = argv[1];
		if (argc == 3) config.chinese = true, config.patch_dir = argv[2];
		ore::Engine engine(platform, config);
		std::string error;
		if (!engine.boot(&error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
		const auto *archive = engine.vfs.archive("SCRIPT");
		if (!archive) return 1;
		std::set<uint32_t> indices;
		int scripts = 0;
		for (const auto &name : archive->names()) {
			if (name.size() < 4 || name.substr(name.size() - 4) != ".SOB") continue;
			std::vector<uint8_t> data;
			if (!engine.vfs.read("SCRIPT/" + name, data)) { ok = false; break; }
			ore::ByteReader file(data);
			std::string magic = file.str(4);
			if (magic == "SOB1") continue;  // global-variable metadata, not executable code
			if (magic != "SOB0" && magic != "SOB2") { ok = false; break; }
			uint32_t hsize = file.u32();
			if (file.bad() || hsize > file.remaining()) { ok = false; break; }
			ore::ByteReader header(data.data() + 8, hsize);
			uint32_t globals = header.u32();
			if (globals > header.remaining() / 8) { ok = false; break; }
			header.skip(size_t(globals) * 8);
			uint32_t natives = header.u32();
			if (natives > header.remaining() / 8) { ok = false; break; }
			for (uint32_t i = 0; i < natives; ++i) { header.u32(); indices.insert(header.u32()); }
			if (header.bad()) { ok = false; break; }
			++scripts;
		}
		if (!scripts || indices.empty()) ok = false;
		for (uint32_t index : indices) {
			if (!engine.vm.native(int(index))) { fprintf(stderr, "missing N_%x\n", index); ok = false; }
		}
		if (ok) printf("native_coverage: %d scripts, %zu referenced natives registered (%s)\n", scripts,
		               indices.size(), config.chinese ? "Chinese overlay" : "original");
		else fprintf(stderr, "FAIL: script/native registration coverage\n");
	}
	std::filesystem::remove_all(saves);
	return ok ? 0 : 1;
}
