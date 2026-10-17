// The game's sound manager (original: DAT_00503630, see
// docs/re/50_audio_movie_input.md): two BGM slots for crossfades, numbered
// channels for SE / system SE / voice, linear fades driven by wall-clock ms,
// BGM ducking under voice, and the "loose" sound objects of N_254..N_268.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/mixer.h"

namespace ore {

struct VoiceCharacter {
	std::string name;   // display name (game encoding)
	std::string name2;
	int volume = 256;   // 0..256
	bool enabled = true;
	bool flag = false;  // +0xd: set at load when the entry has a second string
	std::vector<std::string> prefixes;
};

struct VoiceInfo {
	std::string ext = "OGG";
	struct Prefix {
		std::string prefix;
		int character = 0;
	};
	std::vector<Prefix> prefixes;
	std::vector<VoiceCharacter> characters;
	bool loaded = false;
	bool parse(const std::vector<uint8_t> &data);
};

class SoundManager {
public:
	using Reader = std::function<bool(const std::string &path, std::vector<uint8_t> &out)>;

	SoundManager(AudioMixer &mixer, Reader reader);

	VoiceInfo &voice_info() { return voice_; }
	// Advances fades and the duck; call once per frame.
	void update(int64_t now_ms);

	// --- BGM (N_28a..N_294) ---
	int bgm_play(const std::string &name, bool loop, const std::string &name2);
	void bgm_stop();
	int bgm_fade_in(int ms, const std::string &name, bool loop, const std::string &name2);
	int bgm_fade_out(int ms);
	int bgm_crossfade(int duration, const std::string &name, bool loop, const std::string &name2, int overlap,
	                  int target);
	void bgm_fade_volume(int target, int ms);
	int bgm_playing_count();
	int bgm_fade_type() const { return fade_type_; }
	int bgm_length_ms();
	void bgm_pause(bool paused);
	void set_bgm_volumes(int fade100, int vol256);
	void set_bgm_pan(int pan);
	void set_bgm_enabled(bool on);
	bool bgm_enabled() const { return bgm_enabled_; }
	int bgm_volume() const { return bgm_vol_; }

	// --- channels (N_295..N_2a5) ---
	void allocate_channels(int n);
	void set_channel_volume(int ch, int vol100, int vol256);
	void set_channels_volume(int vol256, bool only_active, int mask);
	void set_channel_pan(int ch, int pan);
	void set_channel_category(int ch, int mask);
	int load(int ch, const std::string &name, bool stream, int lipsync = 0);
	void unload(int ch);
	int play(int ch, bool loop);
	void stop(int ch);
	void fade_in(int ms, const std::string &name, int ch, bool loop, bool stream);
	void fade_out(int ms, int ch);
	void fade_channel_volume(int ch, int target, int ms);
	bool channel_playing(int ch);
	int channel_fade_state(int ch);
	int channel_length_ms(int ch);
	int channel_position_ms(int ch);
	// Lip-sync level of a channel's voice: 0..n-1 (n==0: raw 0..255).
	int channel_lip_level(int ch, int n);
	int load_voice(int ch, const std::string &name, const std::vector<int> &list);
	void duck(int ch, const char *ratio, int config_ratio);

	void set_category_enabled(int mask, bool on);
	bool category_enabled(int mask) const;
	void set_category_default_volume(int which, int vol256);
	int category_default_volume(int which) const;
	// Called when SE are re-enabled: restart looping channels of mask.
	void set_se_enabled(bool on, int mask);

	// --- loose sound objects (N_254..N_268): handles > 0 ---
	int obj_new_static(const std::string &name, int lipsync);
	int obj_new_stream(const std::string &name, const std::string &name2);
	int obj_duplicate(int obj);
	void obj_delete(int obj);
	void obj_play(int obj, bool loop);
	void obj_stop(int obj);
	void obj_set_volume(int obj, int vol256);
	int obj_length_ms(int obj);
	void obj_free_all();
	// Channel sound object handles exported to scripts (N_294, N_2a3): a
	// negative number encodes "channel n" / "bgm" for obj_length_ms.
	static constexpr int kBgmHandle = -1000;

	void stop_all();
	void set_paused(bool paused);

	// Save state (original vtable 0x4ee634 +8/+0xc).
	std::vector<uint8_t> save_state() const;
	void load_state(const std::vector<uint8_t> &data);

private:
	struct Sound {
		std::string name, name2;
		bool stream = false;
		std::shared_ptr<const std::vector<uint8_t>> ogg, ogg2;  // stream sources
		std::shared_ptr<const PcmBuffer> pcm;                   // static source
		std::vector<uint8_t> envelope;                         // lip-sync block levels
		int peak = 0;
		int length_ms = 0;
		int voice = 0;  // mixer voice id while playing
	};
	struct Channel {
		std::unique_ptr<Sound> sound;
		int vol100 = 100, vol256 = 256, pan = 0;
		int state = 0;       // 0 stopped, 1 once, 2 looping
		int fade_state = 0;  // 0 none, 1 volume, 2 in, 3 out
		int category = 1;
		// fade
		int64_t fade_start = 0;
		int fade_ms = 0, fade_from = 0, fade_to = 0, fade_restore = 100;
	};
	struct BgmSlot {
		Sound sound;
		bool loaded = false;
		int fade100 = 100;
		bool loop = false;
		int pause = 0;
	};
	struct Loose {
		std::unique_ptr<Sound> sound;
		int vol256 = 256;
	};

	bool load_sound(Sound &s, const std::string &name, const std::string &name2, bool stream, int lipsync);
	void start_sound(Sound &s, bool loop, float gain, int pan);
	void apply_channel(Channel &c);
	void apply_bgm(BgmSlot &b);
	bool bgm_slot_playing(BgmSlot &b);
	void bgm_open(BgmSlot &b, const std::string &name, bool loop, const std::string &name2);
	void bgm_stop_slot(BgmSlot &b);
	void kill_bgm_tasks();
	Channel *channel(int ch);
	float gain256(int v) const { return v <= 0 ? 0.0f : float(v > 256 ? 256 : v) / 256.0f; }

	AudioMixer &mixer_;
	Reader reader_;
	VoiceInfo voice_;
	int64_t now_ = 0;

	BgmSlot bgm_[2];
	int cur_ = 0;  // current BGM slot
	int bgm_vol_ = 256, bgm_pan_ = 0;
	bool bgm_enabled_ = true;
	bool paused_ = false;
	int fade_type_ = 0;
	// BGM fade task
	struct BgmFade {
		int kind = 0;  // 0 none, 1 volume, 2 in, 3 out, 4 crossfade
		int64_t start = 0;
		int ms = 0, from = 0, to = 0, saved = 100;
		int overlap = 0, target = 0;
	} bgm_fade_;
	// duck task
	struct Duck {
		int state = -1;  // -1 inactive, 1 down, 2 wait voice, 3 hold, 4 up
		int ch = 0, base = 256, target = 128, current = 256;
		int64_t t0 = 0;
	} duck_;
	int duck_bgm_vol() const { return duck_.state >= 0 ? duck_.current : bgm_vol_; }

	std::vector<Channel> channels_;
	bool cat_enabled_[3] = {true, true, true};  // masks 1, 2, 4
	int cat_default_[3] = {-1, -1, -1};
	std::map<int, Loose> loose_;
	int next_loose_ = 1;
};

}  // namespace ore
