#include "audio/sound_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "util/bytes.h"
#include "util/log.h"
#include "util/text.h"

namespace ore {

// ---------------------------------------------------------------- VOICEINF

bool VoiceInfo::parse(const std::vector<uint8_t> &d) {
	ByteReader r(d);
	auto pstr = [&r]() {
		size_t n = r.u8();
		std::string s = r.str(n);
		while (!s.empty() && s.back() == '\0') s.pop_back();
		return s;
	};
	ext = pstr();
	uint32_t nprefix = r.u32();
	uint32_t nchar = r.u32();
	if (r.bad() || nprefix > 10000 || nchar > 1000) return false;
	characters.clear();
	prefixes.clear();
	// The prefix table is built from the per-character lists, in order.
	for (uint32_t c = 0; c < nchar && !r.bad(); ++c) {
		VoiceCharacter vc;
		vc.name = pstr();
		size_t m = r.u8();
		if (m) vc.flag = true;
		if (m > 1) {
			vc.name2 = r.str(m);
			while (!vc.name2.empty() && vc.name2.back() == '\0') vc.name2.pop_back();
		} else {
			r.skip(m);
		}
		uint32_t k = r.u32();
		for (uint32_t i = 0; i < k && !r.bad(); ++i) {
			std::string p = pstr();
			vc.prefixes.push_back(p);
			prefixes.push_back({p, int(c)});
		}
		characters.push_back(std::move(vc));
	}
	if (prefixes.size() != nprefix) log_warn("VOICEINF: %zu prefixes, header says %u", prefixes.size(), nprefix);
	loaded = !r.bad();
	return loaded;
}

// ---------------------------------------------------------------- helpers

SoundManager::SoundManager(AudioMixer &mixer, Reader reader) : mixer_(mixer), reader_(std::move(reader)) {}

static int lerp_ms(int from, int to, int64_t elapsed, int ms) {
	if (ms <= 0 || elapsed >= ms) return to;
	if (elapsed <= 0) return from;
	return from + int((int64_t(to - from) * elapsed) / ms);
}

bool SoundManager::load_sound(Sound &s, const std::string &name, const std::string &name2, bool stream, int lipsync) {
	s = Sound();
	s.name = name;
	s.name2 = name2;
	s.stream = stream;
	std::vector<uint8_t> d;
	if (!reader_(name, d)) {
		log_warn("sound: missing %s", name.c_str());
		return false;
	}
	if (stream) {
		s.ogg = std::make_shared<std::vector<uint8_t>>(std::move(d));
		s.length_ms = ogg_length_ms(*s.ogg);
		if (!name2.empty()) {
			std::vector<uint8_t> d2;
			if (reader_(name2, d2)) {
				s.ogg2 = std::make_shared<std::vector<uint8_t>>(std::move(d2));
				s.length_ms += ogg_length_ms(*s.ogg2);
			}
		}
		return true;
	}
	s.pcm = decode_ogg(d);
	if (!s.pcm) {
		log_warn("sound: cannot decode %s", name.c_str());
		return false;
	}
	s.length_ms = int(uint64_t(s.pcm->frames()) * 1000 / std::max(1, s.pcm->rate));
	if (lipsync) {
		const size_t frames = s.pcm->frames();
		s.envelope.resize((frames + 31) / 32);
		int peak = 1;
		for (size_t b = 0; b < s.envelope.size(); ++b) {
			int64_t sum = 0;
			size_t n = 0;
			for (size_t i = b * 32; i < frames && i < b * 32 + 32; ++i, ++n) sum += std::abs(int(s.pcm->samples[i * 2]));
			int mean = n ? int(sum / int64_t(n)) : 0;
			peak = std::max(peak, mean);
			s.envelope[b] = uint8_t(std::min(255, mean >> 7));
		}
		s.peak = std::max(1, peak >> 7);
	}
	return true;
}

void SoundManager::start_sound(Sound &s, bool loop, float gain, int pan) {
	if (s.voice) mixer_.stop(s.voice);
	AudioMixer::PlayParams p;
	p.volume = gain;
	p.loop = loop;
	ds_pan_gains(pan, p.gain_l, p.gain_r);
	if (s.stream) {
		// intro (name2) once, then the main file
		if (s.ogg2)
			s.voice = mixer_.play_ogg(s.ogg2, s.ogg, p);
		else
			s.voice = mixer_.play_ogg(s.ogg, nullptr, p);
	} else {
		s.voice = mixer_.play_pcm(s.pcm, p);
	}
	if (paused_ && s.voice) mixer_.set_paused(s.voice, true);
}

SoundManager::Channel *SoundManager::channel(int ch) {
	return ch >= 0 && ch < int(channels_.size()) ? &channels_[size_t(ch)] : nullptr;
}

void SoundManager::apply_channel(Channel &c) {
	if (!c.sound || !c.sound->voice) return;
	mixer_.set_volume(c.sound->voice, gain256(c.vol256 * c.vol100 / 100));
	float l, r;
	ds_pan_gains(c.pan, l, r);
	mixer_.set_pan(c.sound->voice, l, r);
}

void SoundManager::apply_bgm(BgmSlot &b) {
	if (!b.sound.voice) return;
	mixer_.set_volume(b.sound.voice, gain256(b.fade100 * duck_bgm_vol() / 100));
	float l, r;
	ds_pan_gains(bgm_pan_, l, r);
	mixer_.set_pan(b.sound.voice, l, r);
}

// ---------------------------------------------------------------- BGM

bool SoundManager::bgm_slot_playing(BgmSlot &b) {
	return b.sound.voice && mixer_.playing(b.sound.voice);
}

void SoundManager::bgm_stop_slot(BgmSlot &b) {
	if (b.sound.voice) mixer_.stop(b.sound.voice);
	b.sound.voice = 0;
}

void SoundManager::bgm_open(BgmSlot &b, const std::string &name, bool loop, const std::string &name2) {
	bgm_stop_slot(b);
	b.loop = loop;
	b.loaded = load_sound(b.sound, name, name2, true, 0);
	if (b.loaded && bgm_enabled_ && b.pause == 0) {
		start_sound(b.sound, loop, gain256(b.fade100 * duck_bgm_vol() / 100), bgm_pan_);
	}
}

int SoundManager::bgm_play(const std::string &name, bool loop, const std::string &name2) {
	BgmSlot &b = bgm_[cur_];
	if (b.loaded && b.sound.name == name && b.sound.name2 == name2 && (bgm_slot_playing(b) || !bgm_enabled_))
		return -1;
	bgm_open(b, name, loop, name2);
	return bgm_enabled_ ? 0 : -2;
}

void SoundManager::kill_bgm_tasks() {
	if (bgm_fade_.kind == 3) {  // fade-out: finish = stop and restore
		bgm_stop_slot(bgm_[cur_]);
		bgm_[cur_].fade100 = bgm_fade_.saved;
	} else if (bgm_fade_.kind == 2) {
		bgm_[cur_].fade100 = bgm_fade_.to;
		apply_bgm(bgm_[cur_]);
	} else if (bgm_fade_.kind == 4) {
		BgmSlot &old = bgm_[cur_ ^ 1];
		bgm_stop_slot(old);
		old.loaded = false;
		bgm_[cur_].fade100 = bgm_fade_.target;
		apply_bgm(bgm_[cur_]);
	}
	bgm_fade_ = BgmFade();
	fade_type_ = 0;
}

void SoundManager::bgm_stop() {
	kill_bgm_tasks();
	for (BgmSlot &b : bgm_) {
		bgm_stop_slot(b);
		b.loaded = false;
		b.sound = Sound();
	}
}

int SoundManager::bgm_fade_in(int ms, const std::string &name, bool loop, const std::string &name2) {
	kill_bgm_tasks();
	BgmSlot &b = bgm_[cur_];
	if (b.loaded && b.sound.name == name && b.sound.name2 == name2 && bgm_slot_playing(b)) return -1;
	int saved = b.fade100;
	b.fade100 = 0;
	bgm_open(b, name, loop, name2);
	bgm_fade_.kind = 2;
	bgm_fade_.start = now_;
	bgm_fade_.ms = ms;
	bgm_fade_.from = 0;
	bgm_fade_.to = saved;
	fade_type_ = 2;
	return 0;
}

int SoundManager::bgm_fade_out(int ms) {
	kill_bgm_tasks();
	BgmSlot &b = bgm_[cur_];
	if (!bgm_slot_playing(b)) {
		bgm_stop_slot(b);
		return -1;
	}
	bgm_fade_.kind = 3;
	bgm_fade_.start = now_;
	bgm_fade_.ms = ms;
	bgm_fade_.from = b.fade100;
	bgm_fade_.to = 0;
	bgm_fade_.saved = b.fade100;
	fade_type_ = 3;
	return 0;
}

int SoundManager::bgm_crossfade(int d, const std::string &name, bool loop, const std::string &name2, int overlap,
                                int target) {
	kill_bgm_tasks();
	BgmSlot &old = bgm_[cur_];
	int f_old = old.fade100;
	if (target < 0) target = f_old;
	cur_ ^= 1;  // the new BGM goes into the other slot
	BgmSlot &nw = bgm_[cur_];
	nw.fade100 = 0;
	int delay = d - std::min(overlap, d);
	if (delay <= 0) bgm_open(nw, name, loop, name2);
	else {
		nw.sound = Sound();
		nw.sound.name = name;
		nw.sound.name2 = name2;
		nw.loop = loop;
		nw.loaded = false;
	}
	bgm_fade_.kind = 4;
	bgm_fade_.start = now_;
	bgm_fade_.ms = d;
	bgm_fade_.from = f_old;
	bgm_fade_.overlap = overlap;
	bgm_fade_.target = target;
	bgm_fade_.saved = f_old;
	fade_type_ = 4;
	return 0;
}

void SoundManager::bgm_fade_volume(int target, int ms) {
	kill_bgm_tasks();
	bgm_fade_.kind = 1;
	bgm_fade_.start = now_;
	bgm_fade_.ms = ms;
	bgm_fade_.from = bgm_[cur_].fade100;
	bgm_fade_.to = target;
	fade_type_ = 1;
}

int SoundManager::bgm_playing_count() {
	int n = 0;
	for (BgmSlot &b : bgm_)
		if (bgm_slot_playing(b)) ++n;
	return n;
}

int SoundManager::bgm_length_ms() {
	return bgm_[cur_].loaded ? bgm_[cur_].sound.length_ms : 0;
}

void SoundManager::bgm_pause(bool paused) {
	for (BgmSlot &b : bgm_) {
		b.pause = std::max(0, b.pause + (paused ? 1 : -1));
		if (b.sound.voice) mixer_.set_paused(b.sound.voice, b.pause > 0 || paused_);
	}
}

void SoundManager::set_bgm_volumes(int fade100, int vol256) {
	if (fade100 >= 0) bgm_[cur_].fade100 = fade100;
	if (vol256 >= 0) {
		bgm_vol_ = vol256;
		duck_.state = -1;
	}
	for (BgmSlot &b : bgm_) apply_bgm(b);
}

void SoundManager::set_bgm_pan(int pan) {
	bgm_pan_ = pan;
	for (BgmSlot &b : bgm_) apply_bgm(b);
}

void SoundManager::set_bgm_enabled(bool on) {
	if (on == bgm_enabled_) return;
	bgm_enabled_ = on;
	BgmSlot &b = bgm_[cur_];
	if (!on) {
		for (BgmSlot &s : bgm_) bgm_stop_slot(s);
	} else if (b.loaded && b.pause == 0) {
		start_sound(b.sound, b.loop, gain256(b.fade100 * duck_bgm_vol() / 100), bgm_pan_);
	} else if (!b.loaded && !b.sound.name.empty()) {
		bgm_open(b, b.sound.name, b.loop, b.sound.name2);
	}
}

// ---------------------------------------------------------------- channels

void SoundManager::allocate_channels(int n) {
	for (Channel &c : channels_)
		if (c.sound && c.sound->voice) mixer_.stop(c.sound->voice);
	channels_.clear();
	channels_.resize(size_t(std::max(0, n)));
}

void SoundManager::set_channel_volume(int ch, int vol100, int vol256) {
	Channel *c = channel(ch);
	if (!c) return;
	if (vol100 >= 0) c->vol100 = vol100;
	if (vol256 >= 0) c->vol256 = vol256;
	apply_channel(*c);
}

void SoundManager::set_channels_volume(int vol256, bool only_active, int mask) {
	for (Channel &c : channels_) {
		if (!(c.category & mask)) continue;
		if (only_active && !(c.state == 2 || (c.sound && c.sound->stream))) continue;
		c.vol256 = vol256;
		apply_channel(c);
	}
}

void SoundManager::set_channel_pan(int ch, int pan) {
	if (Channel *c = channel(ch)) {
		c->pan = pan;
		apply_channel(*c);
	}
}

void SoundManager::set_channel_category(int ch, int mask) {
	if (Channel *c = channel(ch))
		if (mask & 7) c->category = mask & 7;
}

static int cat_index(int mask) {
	return (mask & 4) ? 2 : (mask & 2) ? 1 : 0;
}

int SoundManager::load(int ch, const std::string &name, bool stream, int lipsync) {
	Channel *c = channel(ch);
	if (!c) return -1;
	if (c->sound && c->sound->voice) mixer_.stop(c->sound->voice);
	c->sound = std::make_unique<Sound>();
	c->state = 0;
	c->fade_state = 0;
	c->vol100 = 100;
	int def = category_default_volume(cat_index(c->category));
	if (def >= 0) c->vol256 = def;
	if (!load_sound(*c->sound, name, std::string(), stream, lipsync)) {
		c->sound.reset();
		return -4;
	}
	return 0;
}

void SoundManager::unload(int ch) {
	Channel *c = channel(ch);
	if (!c) return;
	if (c->sound && c->sound->voice) mixer_.stop(c->sound->voice);
	c->sound.reset();
	c->state = 0;
	c->fade_state = 0;
}

int SoundManager::play(int ch, bool loop) {
	Channel *c = channel(ch);
	if (!c || !c->sound) return -1;
	c->fade_state = 0;
	c->state = loop ? 2 : 1;
	if (!category_enabled(c->category)) return -1;
	start_sound(*c->sound, loop, gain256(c->vol256 * c->vol100 / 100), c->pan);
	return 0;
}

void SoundManager::stop(int ch) {
	Channel *c = channel(ch);
	if (!c) return;
	if (c->fade_state == 3) c->vol100 = c->fade_restore;
	c->fade_state = 0;
	if (c->sound && c->sound->voice) {
		mixer_.stop(c->sound->voice);
		c->sound->voice = 0;
	}
	c->state = 0;
}

void SoundManager::fade_in(int ms, const std::string &name, int ch, bool loop, bool stream) {
	Channel *c = channel(ch);
	if (!c) return;
	if (!name.empty() && load(ch, name, stream) != 0) return;
	if (!c->sound) return;
	int target = c->vol100;
	c->vol100 = 0;
	play(ch, loop);
	c->fade_state = 2;
	c->fade_start = now_;
	c->fade_ms = ms;
	c->fade_from = 0;
	c->fade_to = target;
	c->fade_restore = target;
}

void SoundManager::fade_out(int ms, int ch) {
	Channel *c = channel(ch);
	if (!c || !channel_playing(ch)) return;
	c->fade_state = 3;
	c->fade_start = now_;
	c->fade_ms = ms;
	c->fade_from = c->vol100;
	c->fade_to = 0;
	c->fade_restore = c->vol100;
}

void SoundManager::fade_channel_volume(int ch, int target, int ms) {
	Channel *c = channel(ch);
	if (!c) return;
	c->fade_state = 1;
	c->fade_start = now_;
	c->fade_ms = ms;
	c->fade_from = c->vol100;
	c->fade_to = target;
}

bool SoundManager::channel_playing(int ch) {
	Channel *c = channel(ch);
	return c && c->sound && c->sound->voice && mixer_.playing(c->sound->voice);
}

int SoundManager::channel_fade_state(int ch) {
	Channel *c = channel(ch);
	return c ? c->fade_state : 0;
}

int SoundManager::channel_length_ms(int ch) {
	Channel *c = channel(ch);
	return c && c->sound ? c->sound->length_ms : 0;
}

int SoundManager::channel_position_ms(int ch) {
	Channel *c = channel(ch);
	if (!c || !c->sound || !c->sound->voice) return 0;
	return int(mixer_.position_ms(c->sound->voice));
}

int SoundManager::channel_lip_level(int ch, int n) {
	Channel *c = channel(ch);
	if (!c || !c->sound || c->sound->envelope.empty() || !c->sound->voice) return 0;
	int64_t frame = mixer_.position_frames(c->sound->voice);
	size_t b = size_t(std::max<int64_t>(0, frame) / 32);
	if (b >= c->sound->envelope.size()) return 0;
	int env = c->sound->envelope[b];
	if (n == 0) return env;
	return std::min(n - 1, env * n / std::max(1, c->sound->peak / 4));
}

int SoundManager::load_voice(int ch, const std::string &name, const std::vector<int> &list) {
	if (!voice_.loaded) return -5;
	int k = -1;
	for (size_t i = 0; i < voice_.prefixes.size(); ++i) {
		const std::string &p = voice_.prefixes[i].prefix;
		if (name.compare(0, p.size(), p) == 0) {
			k = int(i);
			break;
		}
	}
	if (k < 0) return -3;
	int cidx = voice_.prefixes[size_t(k)].character;
	if (ch == -1) return k;
	if (ch < -1) return cidx;
	VoiceCharacter &vc = voice_.characters[size_t(cidx)];
	if (!category_enabled(4) || !vc.enabled) return -2;
	int j = -1;
	for (size_t i = 0; i < list.size(); ++i)
		if (list[i] == k) {
			j = int(i);
			break;
		}
	std::string path = "VOICE\\" + name + "." + voice_.ext;
	if (load(ch, path, false, j >= 0 ? 2 : 0) != 0) return -4;
	if (Channel *c = channel(ch)) c->vol256 = vc.volume;
	vc.flag = false;
	return list.empty() ? k : j;
}

void SoundManager::duck(int ch, const char *ratio_str, int config_ratio) {
	double ratio;
	if (ratio_str)
		ratio = atof(ratio_str);
	else
		ratio = config_ratio == 50 ? -1.0 : config_ratio * 0.01;
	if (ratio >= 1.0) return;
	if (!channel_playing(ch) || !bgm_playing_count()) return;
	int target = ratio < 0 ? bgm_vol_ >> 1 : int(bgm_vol_ * ratio);
	if (duck_.state == 2 || duck_.state == 3) {
		duck_.ch = ch;
		duck_.state = 2;
		return;
	}
	duck_.state = 1;
	duck_.ch = ch;
	duck_.base = bgm_vol_;
	duck_.target = target;
	duck_.current = bgm_vol_;
	duck_.t0 = now_;
}

void SoundManager::set_category_enabled(int mask, bool on) {
	for (int i = 0; i < 3; ++i)
		if (mask & (1 << i)) cat_enabled_[i] = on;
}

bool SoundManager::category_enabled(int mask) const {
	return cat_enabled_[cat_index(mask)];
}

void SoundManager::set_category_default_volume(int which, int vol256) {
	if (which >= 0 && which < 3) cat_default_[which] = vol256;
}

int SoundManager::category_default_volume(int which) const {
	if (which == 1 && cat_default_[1] == -1) return cat_default_[0];
	if (which == 2 && voice_.loaded) return -2;
	return which >= 0 && which < 3 ? cat_default_[which] : -1;
}

void SoundManager::set_se_enabled(bool on, int mask) {
	cat_enabled_[0] = on;
	for (size_t i = 0; i < channels_.size(); ++i) {
		Channel &c = channels_[i];
		if (!(c.category & mask) || c.state != 2 || !c.sound) continue;
		if (!on) {
			if (c.sound->voice) mixer_.stop(c.sound->voice);
			c.sound->voice = 0;
		} else if (!c.sound->voice) {
			start_sound(*c.sound, true, gain256(c.vol256 * c.vol100 / 100), c.pan);
		}
	}
}

// ---------------------------------------------------------------- loose objects

int SoundManager::obj_new_static(const std::string &name, int lipsync) {
	Loose l;
	l.sound = std::make_unique<Sound>();
	if (!load_sound(*l.sound, name, std::string(), false, lipsync)) return 0;
	int id = next_loose_++;
	loose_[id] = std::move(l);
	return id;
}

int SoundManager::obj_new_stream(const std::string &name, const std::string &name2) {
	Loose l;
	l.sound = std::make_unique<Sound>();
	if (!load_sound(*l.sound, name, name2, true, 0)) return 0;
	int id = next_loose_++;
	loose_[id] = std::move(l);
	return id;
}

int SoundManager::obj_duplicate(int obj) {
	auto it = loose_.find(obj);
	if (it == loose_.end()) return 0;
	Loose l;
	l.sound = std::make_unique<Sound>(*it->second.sound);
	l.sound->voice = 0;
	l.vol256 = it->second.vol256;
	int id = next_loose_++;
	loose_[id] = std::move(l);
	return id;
}

void SoundManager::obj_delete(int obj) {
	auto it = loose_.find(obj);
	if (it == loose_.end()) return;
	if (it->second.sound->voice) mixer_.stop(it->second.sound->voice);
	loose_.erase(it);
}

void SoundManager::obj_play(int obj, bool loop) {
	auto it = loose_.find(obj);
	if (it == loose_.end()) return;
	start_sound(*it->second.sound, loop, gain256(it->second.vol256), 0);
}

void SoundManager::obj_stop(int obj) {
	auto it = loose_.find(obj);
	if (it == loose_.end() || !it->second.sound->voice) return;
	mixer_.stop(it->second.sound->voice);
	it->second.sound->voice = 0;
}

void SoundManager::obj_set_volume(int obj, int vol256) {
	auto it = loose_.find(obj);
	if (it == loose_.end()) return;
	it->second.vol256 = vol256;
	if (it->second.sound->voice) mixer_.set_volume(it->second.sound->voice, gain256(vol256));
}

int SoundManager::obj_length_ms(int obj) {
	if (obj == kBgmHandle) return bgm_length_ms();
	if (obj < 0) return channel_length_ms(-obj - 1);
	auto it = loose_.find(obj);
	return it == loose_.end() ? 0 : it->second.sound->length_ms;
}

void SoundManager::obj_free_all() {
	for (auto &kv : loose_)
		if (kv.second.sound->voice) mixer_.stop(kv.second.sound->voice);
	loose_.clear();
}

void SoundManager::stop_all() {
	bgm_stop();
	for (size_t i = 0; i < channels_.size(); ++i) stop(int(i));
	obj_free_all();
}

void SoundManager::set_paused(bool paused) {
	paused_ = paused;
	mixer_.set_all_paused(paused);
}

// ---------------------------------------------------------------- update

void SoundManager::update(int64_t now_ms) {
	now_ = now_ms;
	// BGM fade task
	BgmFade &f = bgm_fade_;
	if (f.kind) {
		int64_t t = now_ - f.start;
		BgmSlot &b = bgm_[cur_];
		switch (f.kind) {
		case 1:
		case 2:
			b.fade100 = lerp_ms(f.from, f.to, t, f.ms);
			apply_bgm(b);
			if (t >= f.ms || !bgm_slot_playing(b)) f = BgmFade(), fade_type_ = 0;
			break;
		case 3:
			b.fade100 = lerp_ms(f.from, 0, t, f.ms);
			apply_bgm(b);
			if (t >= f.ms) {
				bgm_stop_slot(b);
				b.fade100 = f.saved;
				f = BgmFade();
				fade_type_ = 0;
			}
			break;
		case 4: {
			BgmSlot &old = bgm_[cur_ ^ 1];
			old.fade100 = lerp_ms(f.from, 0, t, f.ms);
			apply_bgm(old);
			if (t >= f.ms && old.sound.voice) {
				bgm_stop_slot(old);
				old.loaded = false;
			}
			int delay = f.ms - std::min(f.overlap, f.ms);
			if (t >= delay) {
				if (!b.loaded && !b.sound.name.empty()) {
					std::string n1 = b.sound.name, n2 = b.sound.name2;
					bgm_open(b, n1, b.loop, n2);
				}
				b.fade100 = lerp_ms(0, f.target, t - delay, f.ms);
				apply_bgm(b);
			}
			if (t >= 2 * int64_t(f.ms) - std::min(f.overlap, f.ms) || (t > delay && !bgm_slot_playing(b))) {
				old.fade100 = f.saved;
				b.fade100 = f.target;
				apply_bgm(b);
				f = BgmFade();
				fade_type_ = 0;
			}
			break;
		}
		}
	}
	// duck task
	if (duck_.state >= 0) {
		int64_t t = now_ - duck_.t0;
		if (!bgm_playing_count()) {
			duck_.state = -1;
		} else if (duck_.state == 1) {
			duck_.current = lerp_ms(duck_.base, duck_.target, t, 500);
			if (t >= 500) duck_.state = 2;
		} else if (duck_.state == 2) {
			if (!channel_playing(duck_.ch)) {
				duck_.state = 3;
				duck_.t0 = now_;
			}
		} else if (duck_.state == 3) {
			if (t >= 1000) {
				duck_.state = 4;
				duck_.t0 = now_;
			}
		} else if (duck_.state == 4) {
			duck_.current = lerp_ms(duck_.target, duck_.base, t, 1000);
			if (t >= 1000) duck_.state = -1;
		}
		for (BgmSlot &b : bgm_) apply_bgm(b);
	}
	// channel fades
	for (size_t i = 0; i < channels_.size(); ++i) {
		Channel &c = channels_[i];
		if (!c.fade_state) continue;
		int64_t t = now_ - c.fade_start;
		bool playing = channel_playing(int(i));
		c.vol100 = lerp_ms(c.fade_from, c.fade_to, t, c.fade_ms);
		apply_channel(c);
		if (c.fade_state == 3 && (t >= c.fade_ms || !playing)) {
			stop(int(i));
			c.vol100 = c.fade_restore;
		} else if (t >= c.fade_ms || !playing) {
			c.fade_state = 0;
		}
	}
	// finished one-shots leave state 1 (the original does not reset it)
}

// ---------------------------------------------------------------- save state

std::vector<uint8_t> SoundManager::save_state() const {
	ByteWriter w;
	w.u32(1);  // version
	const BgmSlot &b = bgm_[cur_];
	w.i32(bgm_pan_);
	w.str(b.loaded || !b.sound.name.empty() ? b.sound.name : std::string());
	w.str(b.sound.name2);
	w.i32(b.fade100);
	w.u8(b.loop);
	w.u32(uint32_t(channels_.size()));
	for (const Channel &c : channels_) {
		w.u8(c.sound ? (c.sound->stream ? 1 : 2) : 0);
		w.str(c.sound ? c.sound->name : std::string());
		w.i32(c.vol100);
		w.i32(c.vol256);
		w.i32(c.pan);
		w.u8(c.state == 2);
		w.u8(uint8_t(c.category));
	}
	return w.take();
}

void SoundManager::load_state(const std::vector<uint8_t> &data) {
	ByteReader r(data);
	if (r.u32() != 1) return;
	stop_all();
	bgm_pan_ = r.i32();
	std::string name = r.str_u32();
	std::string name2 = r.str_u32();
	int fade100 = r.i32();
	bool loop = r.u8();
	cur_ = 0;
	bgm_[0].fade100 = fade100;
	if (!name.empty()) bgm_open(bgm_[0], name, loop, name2);
	uint32_t n = r.u32();
	if (r.bad() || n > 256) return;
	allocate_channels(int(n));
	for (uint32_t i = 0; i < n && !r.bad(); ++i) {
		Channel &c = channels_[i];
		int kind = r.u8();
		std::string cname = r.str_u32();
		int vol100 = r.i32(), vol256 = r.i32(), pan = r.i32();
		bool looping = r.u8();
		c.category = r.u8();
		if (kind) load(int(i), cname, kind == 1);
		c.vol100 = vol100;
		c.vol256 = vol256;
		c.pan = pan;
		if (looping && c.sound) play(int(i), true);
	}
}

}  // namespace ore
