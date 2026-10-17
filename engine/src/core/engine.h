// The engine: owns every subsystem and runs the original's frame loop
// (docs/re/10_lifecycle_system.md §3).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "audio/sound_manager.h"
#include "core/chs_extras.h"
#include "core/object.h"
#include "core/scheduler.h"
#include "gfx/image.h"
#include "gfx/surface.h"
#include "input/input.h"
#include "media/movie.h"
#include "platform/platform.h"
#include "res/vfs.h"
#include "vm/vm.h"

namespace ore {

class TextSystem;
class UiSystem;
class Font;

struct EngineConfig {
	std::string data_dir;
	std::string patch_dir;  // empty = none
	bool chinese = false;
};

class Engine {
public:
	static constexpr int kWidth = 800, kHeight = 600;

	Engine(Platform &platform, EngineConfig config);
	~Engine();

	bool boot(std::string *error);
	// One display frame. Returns 0 to continue, 1 quit requested, -1 fatal.
	int frame(int64_t now_ms);
	const std::string &error() const { return error_; }
	void set_paused(bool paused);

	const Image &framebuffer() const { return frame_; }
	uint64_t frame_serial() const { return frame_serial_; }

	// ---------------------------------------------------------- subsystems
	Platform &platform;
	EngineConfig config;
	TextEncoding encoding;
	Vfs vfs;
	Vm vm;
	AudioMixer mixer;
	SoundManager sound;
	Input input;
	Scheduler tasks;      // DAT_00503214: animations, effects, transitions
	std::unique_ptr<TextSystem> text;
	std::unique_ptr<UiSystem> ui;
	int64_t now = 0;

	bool read_file(const std::string &path, std::vector<uint8_t> &out) { return vfs.read(path, out); }
	ImageRef load_image(const std::string &path, bool keep8);

	// ---------------------------------------------------------- the "Screen" object registry
	int image_tag = 0;   // S+0x1a4
	int sprite_tag = 0;  // S+0x1cc
	std::vector<std::unique_ptr<Object>> images;  // ImageObj and Surface
	std::vector<std::unique_ptr<View>> views;
	std::vector<View *> view_list;  // the render list, descending priority
	std::vector<std::unique_ptr<Layer>> layers;
	template <class T>
	T *own_image(std::unique_ptr<T> o) {
		T *p = o.get();
		images.push_back(std::move(o));
		return p;
	}
	template <class T>
	T *own_layer(std::unique_ptr<T> o) {
		T *p = o.get();
		layers.push_back(std::move(o));
		return p;
	}
	bool delete_image(uint32_t handle);
	bool delete_layer(uint32_t handle);
	void delete_images_by_tag(int tag, int flags);
	void delete_layers_by_tag(int tag, bool invert);
	void insert_view(View *v, int prio);
	// Resolves an image/surface handle to its pixels (type 4 → itself, 7/9/10 → page image).
	ImageRef image_of(uint32_t handle);
	Surface *surface_of(uint32_t handle);
	Surface *main_object = nullptr;  // N_ee (GDI path); unused in our build

	// ---------------------------------------------------------- options block (app+0xe40..0xee3)
	static constexpr int kOptBase = 0xe40, kOptSize = 0xa4;
	uint32_t opt_addr(int app_off) const { return opt_block_ + uint32_t(app_off - kOptBase); }
	uint8_t opt8(int app_off) { return vm.mem().host(opt_addr(app_off))[0]; }
	void set_opt8(int app_off, uint8_t v) { vm.mem().host(opt_addr(app_off))[0] = v; }
	uint32_t opt32(int app_off) { return vm.mem().rd32(opt_addr(app_off)); }
	void set_opt32(int app_off, uint32_t v) { vm.mem().wr32(opt_addr(app_off), v); }
	void save_options();
	void load_options();

	// ---------------------------------------------------------- misc engine state
	std::map<int, uint8_t> app_flags;  // N_4/N_5 store
	int busy = 0;                       // N_13/N_14
	int stopwatch_count = 0;
	std::vector<int64_t> stopwatches;   // N_218..N_21a
	uint32_t rng_state = 0;
	bool quit_requested = false;
	std::string gamedata_name, textlog_name;
	uint32_t snapshot_ = 0;  // unused placeholder

	// Fonts (N_a3..N_bf); default message font (DAT_00503fb0)
	std::vector<std::unique_ptr<Font>> fonts;
	Font *default_font = nullptr;

	// Movie playback (N_271..N_27f)
	std::unique_ptr<MoviePlayer> movie;
	bool movie_fullscreen = false;
	int movie_state = 3;  // 0 stopped, 1 paused, 2 running, 3 initial
	int movie_volume = 256;
	Rect movie_rect{0, 0, kWidth, kHeight};
	std::string movie_name;

	// Saves (save.cpp)
	std::string save_path(const std::string &name) const;
	int save_game(int bank, int slot);
	int load_game(int bank, int slot);
	int write_gamedata();
	int read_gamedata();
	int write_textlog();
	int read_textlog();
	std::vector<uint8_t> state_blob(bool with_log);
	int restore_state_blob(const std::vector<uint8_t> &blob);
	std::vector<int> live_save_vars;  // N_28
	int save_code = 0;
	uint32_t save_title_ptr = 0, save_preview_flag_ptr = 0;
	uint32_t thumb_source = 0, thumb_name_ptr = 0;
	uint32_t thumb_flag_ptr[2] = {0, 0};
	std::vector<uint8_t> mem_snapshot;  // N_31..N_34
	bool has_mem_snapshot = false;
	std::vector<uint8_t> thumbnail(int w, int h);
	// Copies between the three user-variable copies with the original's filters.
	void copy_vars(int from, int to, int scope_sel, int type_sel);

	void request_quit() { quit_requested = true; }

	// Pointer input from the host; presses on the Chinese patch overlay are consumed.
	void mouse_move(int x, int y) { input.on_mouse_move(x, y); }
	void mouse_button(int button, bool down);
	// Native toolbar action; history also closes its own screen.
	void game_command(int command);
	std::unique_ptr<ChsExtras> chs;

private:
	void register_natives();
	void render_frame();
	bool run_vm();

	std::string error_;
	Image frame_{kWidth, kHeight, 32};
	uint64_t frame_serial_ = 1;
	uint64_t rendered_gen_ = 0;
	uint64_t overlay_gen_ = 0;
	bool swallow_release_ = false;
	uint32_t opt_block_ = 0;
	int64_t last_logic_ = -1;
	bool paused_ = false;
	bool booted_ = false;
};

// Native registration, one function per area (core/natives_*.cpp).
void register_system_natives(Engine &e);
void register_save_natives(Engine &e);
void register_audio_natives(Engine &e);
void register_input_natives(Engine &e);
void register_gfx_natives(Engine &e);
void register_text_natives(Engine &e);
void register_ui_natives(Engine &e);
void register_effect_natives(Engine &e);

}  // namespace ore
