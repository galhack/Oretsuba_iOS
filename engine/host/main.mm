// Headless test host: runs the engine on macOS without a window, feeding
// scripted input and writing PNG screenshots.
//
//   orehost --data DIR [--patch DIR --chs] [--save DIR] [--frames N]
//           [--input FILE] [--shot F,F,...] [--shot-every N] [--out DIR]
//           [--trace HEX,HEX,...]
//
// The input file holds one command per line, keyed by frame number:
//   <frame> click X Y        left click (press now, release 3 frames later)
//   <frame> rclick X Y
//   <frame> move X Y
//   <frame> wheel D           +120 up / -120 down
//   <frame> key VK [down|up]  (no state = press and release)
//   <frame> command N        native toolbar command (0 quick save, 1 quick load, ...)
//   <frame> shot NAME
//   <frame> quit
// Times are logical frames (60 per second); the clock advances 16.67 ms per
// frame regardless of real time, so runs are deterministic and fast.
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/engine.h"
#include "platform/apple/apple_platform.h"
#include "stb_image_write.h"
#include "util/log.h"

namespace {

struct Cmd {
	std::string op;
	int a = 0, b = 0;
	std::string s;
};

class HostPlatform : public ore::ApplePlatform {
public:
	using ApplePlatform::ApplePlatform;
	int64_t clock = 0;
	int64_t now_ms() override { return clock; }
	void begin_dialog(const std::string &text, int kind) override {
		fprintf(stderr, "[dialog kind=%d] %s\n", kind, text.c_str());
	}
	int poll_dialog() override { return 1; }
};

void write_png(const ore::Image &img, const std::string &path) {
	std::vector<uint8_t> rgba(size_t(img.width()) * img.height() * 4);
	for (int y = 0; y < img.height(); ++y)
		for (int x = 0; x < img.width(); ++x) {
			uint32_t p = img.row32(y)[x];
			uint8_t *d = &rgba[(size_t(y) * img.width() + x) * 4];
			d[0] = uint8_t(p >> 16), d[1] = uint8_t(p >> 8), d[2] = uint8_t(p), d[3] = 255;
		}
	stbi_write_png(path.c_str(), img.width(), img.height(), 4, rgba.data(), img.width() * 4);
}

}  // namespace

int main(int argc, char **argv) {
	std::string data, patch, save = "/tmp/orehost_save", input, out = "/tmp/orehost";
	bool chs = false;
	std::string trace;  // comma-separated hex native indices
	int frames = 600, shot_every = 0;
	std::set<int> shots;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--data") data = next();
		else if (a == "--patch") patch = next();
		else if (a == "--chs") chs = true;
		else if (a == "--save") save = next();
		else if (a == "--frames") frames = atoi(next().c_str());
		else if (a == "--input") input = next();
		else if (a == "--out") out = next();
		else if (a == "--shot-every") shot_every = atoi(next().c_str());
		else if (a == "--shot") {
			std::stringstream ss(next());
			std::string t;
			while (std::getline(ss, t, ',')) shots.insert(atoi(t.c_str()));
		} else if (a == "--debug") ore::set_log_level(ore::LogLevel::Debug);
		else if (a == "--trace") trace = next();
		else {
			fprintf(stderr, "unknown option %s\n", a.c_str());
			return 2;
		}
	}
	if (data.empty()) {
		fprintf(stderr, "usage: orehost --data DIR [...]\n");
		return 2;
	}
	std::multimap<int, Cmd> cmds;
	if (!input.empty()) {
		std::ifstream f(input);
		std::string line;
		while (std::getline(f, line)) {
			if (line.empty() || line[0] == '#') continue;
			std::stringstream ss(line);
			int frame;
			Cmd c;
			ss >> frame >> c.op;
			if (c.op == "key") {
				ss >> c.a >> c.s;
			} else if (c.op == "shot") {
				ss >> c.s;
			} else {
				ss >> c.a >> c.b;
			}
			cmds.emplace(frame, c);
		}
	}
	mkdir(out.c_str(), 0755);
	HostPlatform platform(save);
	platform.set_chinese_fonts(chs);
	ore::EngineConfig cfg;
	cfg.data_dir = data;
	cfg.patch_dir = patch;
	cfg.chinese = chs;
	ore::Engine engine(platform, cfg);
	std::string err;
	if (!engine.boot(&err)) {
		fprintf(stderr, "boot failed: %s\n", err.c_str());
		return 1;
	}
	{
		std::stringstream ss(trace);
		std::string t;
		while (std::getline(ss, t, ','))
			if (!t.empty()) engine.vm.trace_natives[size_t(strtol(t.c_str(), nullptr, 16)) & 1023] = true;
	}
	std::vector<float> audio(2 * 800);
	std::multimap<int, Cmd> releases;
	int f = 0;
	for (; f < frames; ++f) {
		platform.clock = int64_t(f) * 1000 / 60;
		auto fire = [&](const Cmd &c) {
			if (c.op == "click" || c.op == "rclick") {
				int b = c.op == "click" ? 0 : 1;
				engine.input.on_mouse_move(c.a, c.b);
				engine.mouse_button(b, true);
				Cmd r;
				r.op = "release";
				r.a = b;
				releases.emplace(f + 3, r);
			} else if (c.op == "release") {
				engine.mouse_button(c.a, false);
			} else if (c.op == "move") {
				engine.input.on_mouse_move(c.a, c.b);
			} else if (c.op == "wheel") {
				engine.input.on_wheel(c.a);
			} else if (c.op == "command") {
				const auto *script = engine.vm.current_script();
				fprintf(stderr, "frame %d: command %d in script %s\n", f, c.a, script ? script->name.c_str() : "-");
				engine.game_command(c.a);
			} else if (c.op == "key") {
				if (c.s == "down") engine.input.on_key(c.a, true);
				else if (c.s == "up") engine.input.on_key(c.a, false);
				else {
					engine.input.on_key(c.a, true);
					Cmd r;
					r.op = "keyup";
					r.a = c.a;
					releases.emplace(f + 3, r);
				}
			} else if (c.op == "keyup") {
				engine.input.on_key(c.a, false);
			} else if (c.op == "shot") {
				write_png(engine.framebuffer(), out + "/" + c.s + ".png");
			} else if (c.op == "quit") {
				return false;
			}
			return true;
		};
		bool go = true;
		for (auto range = cmds.equal_range(f); range.first != range.second; ++range.first)
			go = fire(range.first->second) && go;
		for (auto range = releases.equal_range(f); range.first != range.second; ++range.first) fire(range.first->second);
		if (!go) break;
		int r = engine.frame(platform.clock);
		engine.mixer.mix(audio.data(), 735);
		if (shots.count(f) || (shot_every && f % shot_every == 0)) {
			char name[64];
			snprintf(name, sizeof name, "/f%05d.png", f);
			write_png(engine.framebuffer(), out + name);
		}
		if (r != 0) {
			fprintf(stderr, "frame %d: engine stopped (%d) %s\n", f, r, engine.error().c_str());
			break;
		}
	}
	write_png(engine.framebuffer(), out + "/last.png");
	const ore::ScriptBlob *s = engine.vm.current_script();
	fprintf(stderr, "ran %d frames, script %s at %s, steps %llu\n", f, s ? s->name.c_str() : "-",
	        engine.vm.where(engine.vm.pc()).c_str(), (unsigned long long)engine.vm.steps());
	return 0;
}
