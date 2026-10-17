// C interface of the engine for the Swift app.
#include "api/ore_api.h"

#include <cstring>
#include <memory>
#include <string>

#include "core/engine.h"
#include "platform/apple/apple_platform.h"
#include "util/log.h"

struct OreEngine {
	std::unique_ptr<ore::ApplePlatform> platform;
	std::unique_ptr<ore::Engine> engine;
	std::string error;
};

extern "C" {

OreEngine *ore_create(const OreConfig *config, char *err, int err_len) {
	auto e = std::make_unique<OreEngine>();
	e->platform = std::make_unique<ore::ApplePlatform>(config->save_dir ? config->save_dir : "");
	e->platform->set_chinese_fonts(config->chinese != 0);
	ore::EngineConfig cfg;
	cfg.data_dir = config->data_dir ? config->data_dir : "";
	cfg.patch_dir = config->patch_dir ? config->patch_dir : "";
	cfg.chinese = config->chinese != 0 && !cfg.patch_dir.empty();
	e->engine = std::make_unique<ore::Engine>(*e->platform, cfg);
	std::string error;
	if (!e->engine->boot(&error)) {
		if (err && err_len > 0) {
			strncpy(err, error.c_str(), size_t(err_len) - 1);
			err[err_len - 1] = 0;
		}
		return nullptr;
	}
	return e.release();
}

void ore_destroy(OreEngine *e) {
	delete e;
}

int ore_frame(OreEngine *e, int64_t now_ms) {
	int r = e->engine->frame(now_ms);
	if (r < 0) e->error = e->engine->error();
	return r;
}

const char *ore_last_error(OreEngine *e) {
	return e->error.c_str();
}

const uint8_t *ore_framebuffer(OreEngine *e, int *width, int *height) {
	const ore::Image &img = e->engine->framebuffer();
	*width = img.width();
	*height = img.height();
	return img.bits();
}

uint64_t ore_frame_serial(OreEngine *e) {
	return e->engine->frame_serial();
}

void ore_mouse_move(OreEngine *e, int x, int y) {
	e->engine->mouse_move(x, y);
}

void ore_mouse_button(OreEngine *e, int button, int down) {
	e->engine->mouse_button(button, down != 0);
}

void ore_mouse_wheel(OreEngine *e, int delta) {
	e->engine->input.on_wheel(delta);
}

void ore_key(OreEngine *e, int vk, int down) {
	e->engine->input.on_key(vk, down != 0);
}

void ore_game_command(OreEngine *e, int command) {
	e->engine->game_command(command);
}

int ore_audio_rate(void) {
	return ore::AudioMixer::kOutputRate;
}

void ore_audio_mix(OreEngine *e, float *out, int frames) {
	e->engine->mixer.mix(out, frames);
}

void ore_set_ui_callbacks(OreEngine *e, void *ctx, OreDialogCallback dialog, OreTextInputCallback text_input) {
	e->platform->set_dialog_handler([ctx, dialog](const std::string &text, int kind) {
		if (dialog) dialog(ctx, text.c_str(), kind);
	});
	e->platform->set_text_input_handler([ctx, text_input](const std::string &initial) {
		if (text_input) text_input(ctx, initial.c_str());
	});
}

void ore_answer_dialog(OreEngine *e, int result) {
	e->platform->answer_dialog(result);
}

void ore_answer_text_input(OreEngine *e, int ok, const char *utf8) {
	e->platform->answer_text_input(ok != 0, utf8 ? utf8 : "");
}

void ore_set_paused(OreEngine *e, int paused) {
	e->engine->set_paused(paused != 0);
}

#if DEBUG
int ore_debug_start_movie(OreEngine *e, int index) {
	if (index < 0 || index > 1) return 0;
	while (e->engine->chs && e->engine->chs->splash_active()) e->engine->chs->advance_splash();
	e->engine->vm.mem().wr32(e->engine->vm.global_addr(912), uint32_t(index));
	if (!e->engine->vm.start("MOVIEPLAY", &e->error)) return 0;
	log_info("movie diagnostic: index %d, isolated saves", index);
	return 1;
}
#endif

}  // extern "C"
