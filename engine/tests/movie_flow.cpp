// Exercise the same MoviePlay script used by device diagnostics, without skips.
#include <cstdio>
#include <filesystem>
#include <unistd.h>

#include "core/engine.h"
#include "platform/apple/apple_platform.h"

struct Clock : ore::ApplePlatform {
	using ApplePlatform::ApplePlatform;
	int64_t clock = 0;
	int64_t now_ms() override { return clock; }
};

int main(int argc, char **argv) {
	if (argc != 3) { fprintf(stderr, "usage: movie_flow DATA PATCH\n"); return 2; }
	char saves[] = "/tmp/ore-movie-flow-XXXXXX";
	if (!mkdtemp(saves)) return 2;
	bool ok = true;
	for (int index = 0; index < 2; ++index) {
		Clock platform(saves);
		platform.set_chinese_fonts(true);
		ore::EngineConfig config;
		config.data_dir = argv[1];
		config.patch_dir = argv[2];
		config.chinese = true;
		ore::Engine engine(platform, config);
		std::string error;
		if (!engine.boot(&error)) { fprintf(stderr, "boot: %s\n", error.c_str()); ok = false; break; }
		while (engine.chs && engine.chs->splash_active()) engine.chs->advance_splash();
		engine.vm.mem().wr32(engine.vm.global_addr(912), uint32_t(index));
		if (!engine.vm.start("MOVIEPLAY", &error)) { fprintf(stderr, "%s\n", error.c_str()); ok = false; break; }
		bool played = false, completed = false, returned = false;
		uint64_t movieFrames = 0;
		std::vector<float> audio(735 * 2);
		for (int frame = 0; frame < 20000; ++frame) {
			platform.clock = int64_t(frame) * 1000 / 60;
			uint64_t serial = engine.frame_serial();
			int status = engine.frame(platform.clock);
			// 44100 samples / 60 Hz, so the audio and logical clocks advance equally.
			engine.mixer.mix(audio.data(), 735);
			if (engine.movie && engine.movie_state == 2) {
				played = true;
				if (engine.frame_serial() != serial) ++movieFrames;
			}
			if (engine.movie && engine.movie->finished()) completed = true;
			if (status < 0) { fprintf(stderr, "%s\n", engine.error().c_str()); break; }
			if (status > 0) { returned = true; break; }
		}
		if (!played || !completed || !returned || movieFrames < 1000) {
			fprintf(stderr, "FAIL: op%d played=%d completed=%d returned=%d frames=%llu\n", index + 1,
			        played, completed, returned, (unsigned long long)movieFrames);
			ok = false;
		} else {
			printf("op%d: natural completion and script return, %llu rendered frames\n", index + 1,
			       (unsigned long long)movieFrames);
		}
	}
	std::filesystem::remove_all(saves);
	return ok ? 0 : 1;
}
