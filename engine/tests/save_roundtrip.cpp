// Requires player-owned data, but uses only synthetic values in a fresh save directory.
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include "core/engine.h"
#include "platform/apple/apple_platform.h"

static bool check(bool ok, const char *message) {
	if (!ok) fprintf(stderr, "FAIL: %s\n", message);
	return ok;
}

int main(int argc, char **argv) {
	if (argc != 2) { fprintf(stderr, "usage: save_roundtrip DATA_DIR\n"); return 2; }
	char path[] = "/tmp/ore-save-test-XXXXXX";
	if (!mkdtemp(path)) return 1;
	bool ok = true;
	{
		ore::ApplePlatform platform(path);
		ore::EngineConfig config;
		config.data_dir = argv[1];
		ore::Engine engine(platform, config);
		std::string error;
		if (!engine.boot(&error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
		uint32_t scene = 0, global = 0;
		for (const auto &g : engine.vm.globals()) {
			if (g.type != 0 || g.extra < 4) continue;
			if (g.flags == 1 && !scene) scene = g.addr[0];
			if (g.flags == 2 && !global) global = g.addr[0];
		}
		if (!check(scene && global, "fixture has saved and global variables")) return 1;
		engine.vm.mem().wr32(scene, 123456);
		engine.copy_vars(0, 2, 2, 0);
		ok &= check(engine.save_game(0, 0) == 0, "save synthetic scene");
		engine.vm.mem().wr32(scene, 0);
		ok &= check(engine.load_game(0, 0) == 0, "read save from filesystem");
		ok &= check(engine.vm.mem().rd32(scene) == 123456, "restore saved scene value");
		engine.gamedata_name = "GAMEDATA.DAT";
		engine.vm.mem().wr32(global, 654321);
		ok &= check(engine.write_gamedata() == 0, "write global data");
		engine.vm.mem().wr32(global, 0);
		ok &= check(engine.read_gamedata() == 0, "read global data from filesystem");
		ok &= check(engine.vm.mem().rd32(global) == 654321, "restore global value");
		engine.textlog_name = "TEXT_LOG.DAT";
		auto &history = engine.vm.init_table();
		size_t historyIndex = 0;
		while (historyIndex < history.size() && history[historyIndex].bits_a.empty()) ++historyIndex;
		if (!check(historyIndex < history.size(), "fixture has read-history bits")) return 1;
		history[historyIndex].bits_a[0] = 1;
		ok &= check(engine.write_textlog() == 0, "write read history");
		history[historyIndex].bits_a[0] = 0;
		ok &= check(engine.read_textlog() == 0, "read history from filesystem");
		ok &= check(history[historyIndex].bits_a[0] == 1, "restore read-history bit");
	}
	std::filesystem::remove_all(path);
	if (ok) puts("save_roundtrip: scene, global data and read history passed");
	return ok ? 0 : 1;
}
