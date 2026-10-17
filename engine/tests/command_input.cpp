// Native toolbar actions must reach script commands without keyboard bindings.
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include "core/engine.h"
#include "platform/apple/apple_platform.h"

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char saves[] = "/tmp/ore-command-test-XXXXXX";
    if (!mkdtemp(saves)) return 2;
    bool ok = true;
    {
        ore::ApplePlatform platform(saves);
        ore::EngineConfig config;
        config.data_dir = argv[1];
        ore::Engine engine(platform, config);
        std::string error;
        if (!engine.boot(&error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
        const auto args = engine.vm.mem().alloc(8, "command test");
        engine.vm.mem().wr32(args, 6);
        engine.vm.mem().wr32(args + 4, 0);
        auto first = engine.vm.native(0x222);
        auto check = [&](bool value, const char *message) {
            if (!value) { fprintf(stderr, "FAIL: %s\n", message); ok = false; }
        };
        // Disable all configurable shortcut keys: native actions must still work.
        for (int i = 0; i < 12; ++i) engine.set_opt8(0xeac + i, 0);
        // The script remaps logical shortcuts through G[868] before dispatch.
        // Use an intentionally non-identity layout to exercise semantic actions.
        const int dispatch[] = {3, 4, 0, 1, 2, 9, 8, 10, 7, 5, 6, 11};
        auto mapping = engine.vm.global_addr(868);
        for (int i = 0; i < 12; ++i) engine.vm.mem().wr32(mapping + i * 4, dispatch[i]);
        const int actions[][2] = {{0, 8}, {1, 9}, {3, 6}, {4, 7}, {7, 14}, {8, 12}};
        for (const auto &action : actions) {
            int command = action[0];
            engine.input.on_command(command);
            engine.input.poll();
            check(first(engine.vm, args) == action[1], "toolbar action respects the script command mapping");
            engine.input.clear_edges();
            check(first(engine.vm, args) == -1, "toolbar action is consumed once");
            engine.input.poll();
            check(first(engine.vm, args) == -1, "toolbar action does not repeat next frame");
        }
        engine.input.on_command(-1);
        engine.input.on_command(12);
        engine.input.poll();
        check(first(engine.vm, args) == -1, "invalid commands are ignored");
        engine.input.on_command(3);
        engine.input.clear();
        engine.input.poll();
        check(first(engine.vm, args) == -1, "clearing input discards pending commands");
        check(engine.vm.start("HISTORY", &error), "history script can be selected");
        engine.game_command(7);
        engine.input.poll();
        check(engine.input.key_pressed(0x01), "history button cancels an open history screen");
        check(first(engine.vm, args) == -1, "history close does not also reopen history");
        engine.input.poll();
        check(!engine.input.key_pressed(0x01) && !engine.input.key_held(0x01),
              "history close releases cancel after one frame");
        check(engine.vm.start("MAIN", &error), "main script can be selected");
        engine.game_command(7);
        engine.input.poll();
        check(first(engine.vm, args) == 14, "history button still opens history outside its screen");
    }
    std::filesystem::remove_all(saves);
    if (ok) puts("command_input: native actions dispatch once without keyboard bindings");
    return ok ? 0 : 1;
}
