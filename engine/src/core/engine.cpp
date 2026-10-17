#include "core/engine.h"

#include <algorithm>
#include <cstring>

#include "gfx/blend.h"
#include "gfx/transition.h"
#include "text/font.h"
#include "text/text_system.h"
#include "ui/ui_system.h"
#include "util/bytes.h"
#include "util/log.h"

namespace ore {
namespace {

// Option block defaults from the app constructor (0x4116f0), app+0xe40..0xee3.
void default_options(uint8_t *o) {
	auto b = [o](int off, uint8_t v) { o[off - Engine::kOptBase] = v; };
	auto d = [o](int off, uint32_t v) { memcpy(o + off - Engine::kOptBase, &v, 4); };
	memset(o, 0, Engine::kOptSize);
	b(0xe40, 1), b(0xe44, 1), b(0xe45, 1), b(0xe47, 1), b(0xe49, 1), b(0xe4d, 1);
	d(0xe50, 750);
	b(0xe54, 1), b(0xe56, 30), b(0xe57, 1), b(0xe58, 30), b(0xe5a, 50), b(0xe5b, 1);
	d(0xe5c, 1000);
	// e60: message font face "ＭＳ ゴシック" (cp932)
	static const char kGothic[] = "\x82\x6c\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4e";
	memcpy(o + 0xe60 - Engine::kOptBase, kGothic, sizeof kGothic);
	b(0xe80, 1), b(0xe82, 2), b(0xe85, 1), b(0xe86, 0x8c);
	b(0xe8d, 1), b(0xe8e, 1), b(0xe8f, 1), b(0xe90, 1);
	d(0xe94, 0x96), d(0xe98, 0x100), d(0xe9c, 0x100), d(0xea0, 0x100);
	b(0xea4, 1), b(0xea9, 0x41);
	const uint8_t keys[] = {0x2c, 0x2d, 0x1e, 0x41, 0x42, 0x43, 0x44, 0x39, 0x32, 0x2e, 0xd2, 0x1f, 0x0e, 0x14};
	for (int i = 0; i < 14; ++i) b(0xeaa + i, keys[i]);
	b(0xeb8, 1), b(0xec9, 1), b(0xecd, 1), b(0xece, 1), b(0xecf, 1), b(0xed1, 1), b(0xed2, 1);
	d(0xed4, 0x100);
	b(0xed8, 1), b(0xeda, 1), b(0xedc, 5), b(0xee2, 1);
}

}  // namespace

Engine::Engine(Platform &p, EngineConfig cfg)
    : platform(p),
      config(std::move(cfg)),
      encoding(config.chinese ? TextEncoding::Gbk : TextEncoding::ShiftJis),
      vm([this](const std::string &path, std::vector<uint8_t> &out) { return vfs.read(path, out); }),
      sound(mixer, [this](const std::string &path, std::vector<uint8_t> &out) { return vfs.read(path, out); }) {}

Engine::~Engine() {
	if (booted_) {
		write_gamedata();
		write_textlog();
		save_options();
	}
	mixer.stop_all();
	ui.reset();
	text.reset();
	tasks.clear();
	layers.clear();
	views.clear();
	images.clear();
}

ImageRef Engine::load_image(const std::string &path, bool keep8) {
	std::vector<uint8_t> d;
	if (!vfs.read(path, d)) {
		log_warn("image: missing %s", path.c_str());
		return nullptr;
	}
	ImageRef img = Image::from_elg(d, keep8);
	if (!img) log_warn("image: cannot decode %s", path.c_str());
	return img;
}

bool Engine::boot(std::string *err) {
	if (!vfs.mount_archives(config.data_dir, err)) return false;
	if (config.chinese && !config.patch_dir.empty()) {
		vfs.add_overlay(config.patch_dir + "/SCRIPT_CHS", "SCRIPT");
		vfs.add_overlay(config.patch_dir + "/SYS", "SYS");
		vfs.add_overlay(config.patch_dir + "/DATA", "DATA");
	}
	register_natives();
	if (config.chinese && !config.patch_dir.empty()) {
		chs = std::make_unique<ChsExtras>();
		if (!chs->load(config.patch_dir + "/ORE_TUBA_CHS.dll", platform)) chs.reset();
	}
	if (chs) {
		// let the overlay see every message record (the patch hooked the same spot)
		NativeFn set_message = vm.native(0x1ce);
		vm.set_native(0x1ce, [this, set_message](Vm &v, uint32_t a) {
			uint32_t rec = v.mem().rd32(a);
			if (rec) {
				std::string text;
				uint32_t p = rec + 3;
				for (;;) {
					uint8_t c = v.mem().host(p)[0];
					if (!c) {
						// the name terminator '$' may already have been replaced by NUL
						uint8_t n = v.mem().host(p + 1)[0];
						if (n == '$' || n == '&') {
							text.push_back('$');
							++p;
							continue;
						}
						break;
					}
					text.push_back(char(c));
					++p;
					if (text.size() > 4096) break;
				}
				chs->on_message(text);
			}
			return set_message ? set_message(v, a) : 0;
		}, "set_message");
	}
	if (!vm.init(err)) return false;
	opt_block_ = vm.mem().alloc(kOptSize + 16, "options");
	default_options(vm.mem().host(opt_block_, kOptSize));
	load_options();
	std::vector<uint8_t> d;
	if (vfs.read("SCRIPT\\VOICEINF.DAT", d)) sound.voice_info().parse(d);
	text = std::make_unique<TextSystem>(*this);
	ui = std::make_unique<UiSystem>(*this);
	// FUN_0040b8f0: push the sound options into the sound manager
	sound.set_bgm_enabled(opt8(0xe8d));
	sound.set_category_enabled(1, opt8(0xe8e));
	sound.set_category_enabled(2, opt8(0xe8f));
	sound.set_category_enabled(4, opt8(0xe90));
	sound.set_bgm_volumes(-1, int(opt32(0xe94)));
	sound.set_category_default_volume(0, int(opt32(0xe98)));
	sound.set_category_default_volume(1, int(opt32(0xe9c)));
	rng_state = uint32_t(platform.now_ms()) | 1;
	if (!vm.start("MAIN", err)) return false;
	booted_ = true;
	return true;
}

void Engine::set_paused(bool p) {
	paused_ = p;
	sound.set_paused(p);
	if (p) {
		write_gamedata();
		write_textlog();
		save_options();
	}
}

bool Engine::run_vm() {
	Vm::RunResult r = vm.run_frame();
	if (r == Vm::RunResult::Error) {
		error_ = "script error: " + vm.error();
		return false;
	}
	if (r == Vm::RunResult::Finished) quit_requested = true;
	return true;
}

int Engine::frame(int64_t t) {
	if (!booted_) return -1;
	if (paused_) return 0;
	if (last_logic_ < 0) last_logic_ = t - 16;
	int64_t elapsed = t - last_logic_;
	// Keep input pending until at least one logical step will run.
	if (elapsed < 16) return quit_requested ? 1 : 0;  // 60 Hz logic on faster displays
	int steps = int(elapsed / 16);
	if (steps > 5) steps = 1, last_logic_ = t;
	else last_logic_ += int64_t(steps) * 16667 / 1000;
	now = t;
	input.poll();
	sound.update(now);
	tasks.tick(now);
	for (int i = 0; i < steps; ++i) {
		if (!run_vm()) return -1;
		if (quit_requested) break;
		if (i == 0) input.clear_edges();
	}
	render_frame();
	return quit_requested ? 1 : 0;
}

void Engine::render_frame() {
	if (movie && movie_fullscreen && movie_state == 2) {
		if (movie->update(now)) {
			const auto &f = movie->frame();
			Image src(movie->width(), movie->height(), 32);
			memcpy(src.bits(), f.data(), f.size());
			fill_rect(frame_, frame_.rect(), 0);
			stretch_blit(frame_, src, movie_rect.l, movie_rect.t, movie_rect.w(), movie_rect.h(), src.rect(), 0, 2, 255,
			             false);
			++frame_serial_;
		}
		if (movie->finished()) movie_state = 0;
		return;
	}
	// end finished transitions (their last frame is the new page content)
	for (auto &o : images)
		if (auto *s = dynamic_cast<Surface *>(o.get()))
			if (s->transition && s->transition->done) s->end_transition(s->trans_apply);
	uint64_t overlay = chs ? chs->generation() : 0;
	if (rendered_gen_ == g_gfx_generation && overlay == overlay_gen_) {
		bool animating = false;
		for (View *v : view_list)
			if (v->visible && v->surface && v->surface->transition) animating = true;
		if (!animating) return;
	}
	fill_rect(frame_, frame_.rect(), 0);
	for (View *v : view_list) {
		if (!v->visible || !v->surface) continue;
		v->surface->update(now);
		Image &img = v->surface->current_image();
		if (v->dw > 0 && v->dh > 0)
			stretch_blit(frame_, img, v->x, v->y, v->dw, v->dh, v->src, 0, 0, 255, false);
		else
			blit(frame_, img, v->x, v->y, v->src, 0, 255, false);
	}
	if (view_list.empty() && main_object) {
		main_object->update(now);
		blit(frame_, main_object->current_image(), 0, 0, main_object->image().rect(), 0, 255, false);
	}
	if (chs) chs->draw(frame_);
	overlay_gen_ = overlay;
	rendered_gen_ = g_gfx_generation;
	++frame_serial_;
}

void Engine::game_command(int command) {
	const ScriptBlob *script = vm.current_script();
	if (command == 7 && script && script->name == "HISTORY") {
		// HISTORY polls Cancel rather than the ADV command dispatcher.
		input.on_key(0x1B, true);
		input.on_key(0x1B, false);
	} else {
		input.on_command(command);
	}
}

void Engine::mouse_button(int button, bool down) {
	if (button == 0 && chs) {
		if (down && chs->on_press(input.cursor_x(), input.cursor_y())) {
			swallow_release_ = true;
			return;
		}
		if (!down && swallow_release_) {
			swallow_release_ = false;
			return;
		}
	}
	input.on_mouse_button(button, down);
}

// ---------------------------------------------------------------- object registry

bool Engine::delete_image(uint32_t h) {
	for (auto it = images.begin(); it != images.end(); ++it) {
		if ((*it)->handle() != h) continue;
		Surface *s = dynamic_cast<Surface *>(it->get());
		if (s) {
			for (auto &v : views)
				if (v->surface == s) v->surface = nullptr;
			for (auto &o : images)
				if (auto *o2 = dynamic_cast<Surface *>(o.get()))
					if (o2->linked == s) o2->linked = nullptr;
			if (main_object == s) main_object = nullptr;
		}
		images.erase(it);
		gfx_touch();
		return true;
	}
	return false;
}

bool Engine::delete_layer(uint32_t h) {
	for (auto it = layers.begin(); it != layers.end(); ++it) {
		if ((*it)->handle() == h) {
			layers.erase(it);
			gfx_touch();
			return true;
		}
	}
	return false;
}

void Engine::delete_images_by_tag(int tag, int flags) {
	bool invert = flags & 2;
	auto match = [&](int t) { return invert ? t != tag : t == tag; };
	for (size_t i = 0; i < views.size();) {
		if (match(views[i]->tag)) {
			View *v = views[i].get();
			view_list.erase(std::remove(view_list.begin(), view_list.end(), v), view_list.end());
			views.erase(views.begin() + long(i));
		} else {
			++i;
		}
	}
	if (flags & 1) {
		std::vector<uint32_t> doomed;
		for (auto &o : images) {
			int t = 0;
			if (auto *io = dynamic_cast<ImageObj *>(o.get())) t = io->image ? io->image->tag : 0;
			if (auto *s = dynamic_cast<Surface *>(o.get())) t = s->image().tag;
			if (match(t)) doomed.push_back(o->handle());
		}
		for (uint32_t h : doomed) delete_image(h);
	}
	gfx_touch();
}

void Engine::delete_layers_by_tag(int tag, bool invert) {
	layers.erase(std::remove_if(layers.begin(), layers.end(),
	                            [&](const std::unique_ptr<Layer> &l) { return invert ? l->tag != tag : l->tag == tag; }),
	             layers.end());
	gfx_touch();
}

void Engine::insert_view(View *v, int prio) {
	view_list.erase(std::remove(view_list.begin(), view_list.end(), v), view_list.end());
	v->prio = prio < 0 ? 0 : prio;
	auto it = view_list.begin();
	while (it != view_list.end() && (*it)->prio >= v->prio) ++it;
	view_list.insert(it, v);
	v->in_screen = true;
	gfx_touch();
}

ImageRef Engine::image_of(uint32_t h) {
	Object *o = Object::from_handle(h);
	if (!o) return nullptr;
	if (auto *io = dynamic_cast<ImageObj *>(o)) return io->image;
	if (auto *s = dynamic_cast<Surface *>(o)) return s->image_ref();
	return nullptr;
}

Surface *Engine::surface_of(uint32_t h) {
	return Object::as<Surface>(h);
}

// ---------------------------------------------------------------- options file

std::string Engine::save_path(const std::string &name) const {
	return platform.save_dir() + "/" + name;
}

void Engine::save_options() {
	if (!opt_block_) return;
	FILE *f = fopen(save_path("APP_CFG.DAT").c_str(), "wb");
	if (!f) return;
	uint32_t n = kOptSize;
	fwrite(&n, 4, 1, f);
	fwrite(vm.mem().host(opt_block_, kOptSize), 1, kOptSize, f);
	fclose(f);
}

void Engine::load_options() {
	FILE *f = fopen(save_path("APP_CFG.DAT").c_str(), "rb");
	if (!f) return;
	uint32_t n = 0;
	if (fread(&n, 4, 1, f) == 1 && n <= kOptSize) {
		std::vector<uint8_t> buf(n);
		if (fread(buf.data(), 1, n, f) == n) memcpy(vm.mem().host(opt_block_, kOptSize), buf.data(), n);
	}
	fclose(f);
}

void Engine::register_natives() {
	register_system_natives(*this);
	register_save_natives(*this);
	register_audio_natives(*this);
	register_input_natives(*this);
	register_gfx_natives(*this);
	register_text_natives(*this);
	register_ui_natives(*this);
	register_effect_natives(*this);
}

}  // namespace ore
