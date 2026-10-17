// Requires player-owned archives; progress and titles below are synthetic.
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include "core/engine.h"
#include "platform/apple/apple_platform.h"

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char saves[] = "/tmp/ore-quick-save-XXXXXX";
    if (!mkdtemp(saves)) return 2;
    bool ok = true;
    {
        ore::ApplePlatform platform(saves);
        ore::EngineConfig config;
        config.data_dir = argv[1];
        ore::Engine engine(platform, config);
        std::string error;
        if (!engine.boot(&error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
        auto check = [&](bool value, const char *message) {
            if (!value) { fprintf(stderr, "FAIL: %s\n", message); ok = false; }
        };
        uint32_t scene = 0;
        for (const auto &g : engine.vm.globals())
            if (g.flags == 1 && g.type == 0 && g.extra >= 4) { scene = g.addr[0]; break; }
        if (!scene) return 1;
        for (int i = 1; i <= 7; ++i) {
            engine.vm.mem().wr32(scene, uint32_t(i * 10));
            engine.copy_vars(0, 2, 2, 0);
            engine.save_title_ptr = engine.vm.mem().alloc_string(i == 7 ? "latest wrapped save" : "older save");
            check(engine.save_game(11, -1) == 0, "quick save writes through ring rollover");
        }
        auto output = engine.vm.mem().alloc(1024, "save description");
        auto args = engine.vm.mem().alloc(16, "description args");
        engine.vm.mem().wr32(args, output);
        engine.vm.mem().wr32(args + 4, 11);
        engine.vm.mem().wr32(args + 8, uint32_t(-1));
        engine.vm.mem().wr32(args + 12, 1);
        auto describe = engine.vm.native(0x2c);
        auto next = engine.opt8(0xedc);
        check(describe(engine.vm, args) == 0, "quick load confirmation describes the newest ring save");
        check(std::string(engine.vm.mem().cstr(output)).find("latest wrapped save") != std::string::npos,
              "confirmation refers to the newest save after rollover");
        check(engine.opt8(0xedc) == next, "reading a description does not advance the ring");
        engine.vm.mem().wr32(scene, 0);
        check(engine.load_game(11, -1) == 0, "quick load reads the same newest save");
        check(engine.vm.mem().rd32(scene) == 70, "quick load restores the newest state");
        for (int slot = 5; slot <= 9; ++slot) {
            char name[32];
            snprintf(name, sizeof name, "DATA11%02d.SAV", slot);
            std::filesystem::remove(std::filesystem::path(saves) / name);
        }
        check(describe(engine.vm, args) == -1, "an empty ring reports no save");
    }
    std::filesystem::remove_all(saves);
    if (ok) puts("quick_save: newest confirmation, rollover, load and empty ring passed");
    return ok ? 0 : 1;
}
