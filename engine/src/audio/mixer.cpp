#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace ore {

std::shared_ptr<PcmBuffer> decode_ogg(const std::vector<uint8_t> &data) {
	int channels = 0, rate = 0;
	short *out = nullptr;
	int frames = stb_vorbis_decode_memory(data.data(), int(data.size()), &channels, &rate, &out);
	if (frames < 0 || !out) return nullptr;
	auto pcm = std::make_shared<PcmBuffer>();
	pcm->rate = rate;
	pcm->samples.resize(size_t(frames) * 2);
	for (int i = 0; i < frames; ++i) {
		pcm->samples[size_t(i) * 2] = out[size_t(i) * channels];
		pcm->samples[size_t(i) * 2 + 1] = out[size_t(i) * channels + (channels > 1 ? 1 : 0)];
	}
	free(out);
	return pcm;
}

int ogg_length_ms(const std::vector<uint8_t> &data) {
	int err = 0;
	stb_vorbis *v = stb_vorbis_open_memory(data.data(), int(data.size()), &err, nullptr);
	if (!v) return 0;
	unsigned frames = stb_vorbis_stream_length_in_samples(v);
	unsigned rate = stb_vorbis_get_info(v).sample_rate;
	stb_vorbis_close(v);
	return rate ? int(uint64_t(frames) * 1000 / rate) : 0;
}

void ds_pan_gains(int pan, float &left, float &right) {
	pan = std::max(-10000, std::min(10000, pan));
	left = right = 1.0f;
	if (pan > 0) left = float(std::pow(10.0, -pan / 2000.0));
	if (pan < 0) right = float(std::pow(10.0, pan / 2000.0));
}

struct AudioMixer::Voice {
	enum Kind { Ogg, Pcm, Stream } kind = Ogg;
	int id = 0;
	PlayParams params;
	// Ogg
	stb_vorbis *vorbis = nullptr;
	std::shared_ptr<const std::vector<uint8_t>> data, next;
	int channels = 2;
	// Pcm
	std::shared_ptr<const PcmBuffer> pcm;
	size_t pcm_pos = 0;
	// Stream
	std::deque<int16_t> queue;
	int64_t consumed = 0;

	int rate = AudioMixer::kOutputRate;
	std::vector<float> buf;  // decoded source frames, stereo
	size_t buf_frames = 0;
	double frac = 0.0;
	int64_t file_pos = 0;    // source frame index of buf[0] in the current file
	int64_t played = 0;      // output frames mixed
	bool eof = false, paused = false, done = false;

	~Voice() {
		if (vorbis) stb_vorbis_close(vorbis);
	}

	bool open_vorbis(std::shared_ptr<const std::vector<uint8_t>> d) {
		if (vorbis) stb_vorbis_close(vorbis);
		vorbis = nullptr;
		int err = 0;
		vorbis = stb_vorbis_open_memory(d->data(), int(d->size()), &err, nullptr);
		if (!vorbis) return false;
		data = std::move(d);
		stb_vorbis_info info = stb_vorbis_get_info(vorbis);
		channels = info.channels;
		rate = int(info.sample_rate);
		return true;
	}

	size_t read_source(float *dst, size_t want) {
		switch (kind) {
		case Pcm: {
			size_t got = 0;
			while (got < want) {
				if (pcm_pos >= pcm->frames()) {
					if (!params.loop) break;
					pcm_pos = 0;
					file_pos = -int64_t(buf_frames + got);
				}
				size_t n = std::min(want - got, pcm->frames() - pcm_pos);
				const int16_t *s = &pcm->samples[pcm_pos * 2];
				for (size_t i = 0; i < n * 2; ++i) dst[got * 2 + i] = s[i] / 32768.0f;
				got += n;
				pcm_pos += n;
			}
			return got;
		}
		case Stream: {
			size_t n = std::min(want, queue.size() / 2);
			for (size_t i = 0; i < n * 2; ++i) {
				dst[i] = queue.front() / 32768.0f;
				queue.pop_front();
			}
			consumed += int64_t(n);
			return n;
		}
		case Ogg: {
			size_t got = 0;
			while (got < want && vorbis) {
				float tmp[2 * 1024];
				int n = int(std::min<size_t>(want - got, 1024));
				int r;
				if (channels == 1) {
					r = stb_vorbis_get_samples_float_interleaved(vorbis, 1, tmp, n);
					for (int i = r; i-- > 0;) tmp[i * 2] = tmp[i * 2 + 1] = tmp[i];
				} else {
					r = stb_vorbis_get_samples_float_interleaved(vorbis, 2, tmp, n * 2);
				}
				if (r > 0) {
					memcpy(dst + got * 2, tmp, size_t(r) * 2 * sizeof(float));
					got += size_t(r);
					continue;
				}
				// end of the current file
				if (next) {
					auto n2 = std::move(next);
					next.reset();
					if (!open_vorbis(n2)) break;
					file_pos = -int64_t(buf_frames + got);
				} else if (params.loop) {
					stb_vorbis_seek_start(vorbis);
					file_pos = -int64_t(buf_frames + got);
				} else {
					break;
				}
			}
			return got;
		}
		}
		return 0;
	}

	void fill(size_t need) {
		size_t start = size_t(frac);
		if (start > 0) {
			size_t keep = buf_frames > start ? buf_frames - start : 0;
			memmove(buf.data(), buf.data() + start * 2, keep * 2 * sizeof(float));
			buf_frames = keep;
			frac -= double(start);
			file_pos += int64_t(start);
		}
		if (buf.size() < (need + 64) * 2) buf.resize((need + 64) * 2);
		if (buf_frames < need && !eof) {
			size_t got = read_source(&buf[buf_frames * 2], need - buf_frames);
			buf_frames += got;
			if (got == 0 && kind != Stream) eof = true;
		}
	}
};

AudioMixer::AudioMixer() = default;
AudioMixer::~AudioMixer() = default;

AudioMixer::Voice *AudioMixer::find(int id) {
	for (auto &v : voices_)
		if (v->id == id && !v->done) return v.get();
	return nullptr;
}

int AudioMixer::add(std::unique_ptr<Voice> v) {
	std::lock_guard<std::mutex> lock(mu_);
	v->id = next_id_++;
	int id = v->id;
	voices_.push_back(std::move(v));
	return id;
}

int AudioMixer::play_ogg(std::shared_ptr<const std::vector<uint8_t>> data,
                         std::shared_ptr<const std::vector<uint8_t>> next, const PlayParams &params) {
	if (!data || data->empty()) return 0;
	auto v = std::make_unique<Voice>();
	v->kind = Voice::Ogg;
	v->params = params;
	if (!v->open_vorbis(std::move(data))) return 0;
	v->next = std::move(next);
	return add(std::move(v));
}

int AudioMixer::play_pcm(std::shared_ptr<const PcmBuffer> pcm, const PlayParams &params) {
	if (!pcm || pcm->frames() == 0) return 0;
	auto v = std::make_unique<Voice>();
	v->kind = Voice::Pcm;
	v->params = params;
	v->rate = pcm->rate;
	v->pcm = std::move(pcm);
	return add(std::move(v));
}

int AudioMixer::open_stream(int sample_rate, float volume) {
	auto v = std::make_unique<Voice>();
	v->kind = Voice::Stream;
	v->rate = sample_rate;
	v->params.volume = volume;
	return add(std::move(v));
}

void AudioMixer::push_pcm(int id, const int16_t *frames, size_t count) {
	std::lock_guard<std::mutex> lock(mu_);
	if (Voice *v = find(id)) v->queue.insert(v->queue.end(), frames, frames + count * 2);
}

size_t AudioMixer::queued_frames(int id) {
	std::lock_guard<std::mutex> lock(mu_);
	Voice *v = find(id);
	return v ? v->queue.size() / 2 : 0;
}

int64_t AudioMixer::consumed_frames(int id) {
	std::lock_guard<std::mutex> lock(mu_);
	Voice *v = find(id);
	return v ? v->consumed : 0;
}

void AudioMixer::stop(int id) {
	std::lock_guard<std::mutex> lock(mu_);
	if (Voice *v = find(id)) v->done = true;
}

void AudioMixer::set_volume(int id, float volume) {
	std::lock_guard<std::mutex> lock(mu_);
	if (Voice *v = find(id)) v->params.volume = volume;
}

void AudioMixer::set_pan(int id, float gain_l, float gain_r) {
	std::lock_guard<std::mutex> lock(mu_);
	if (Voice *v = find(id)) {
		v->params.gain_l = gain_l;
		v->params.gain_r = gain_r;
	}
}

void AudioMixer::set_paused(int id, bool paused) {
	std::lock_guard<std::mutex> lock(mu_);
	if (Voice *v = find(id)) v->paused = paused;
}

bool AudioMixer::playing(int id) {
	std::lock_guard<std::mutex> lock(mu_);
	return find(id) != nullptr;
}

int64_t AudioMixer::position_ms(int id) {
	std::lock_guard<std::mutex> lock(mu_);
	Voice *v = find(id);
	return v ? v->played * 1000 / kOutputRate : 0;
}

int64_t AudioMixer::position_frames(int id) {
	std::lock_guard<std::mutex> lock(mu_);
	Voice *v = find(id);
	return v ? v->file_pos + int64_t(v->frac) : 0;
}

void AudioMixer::set_master_volume(float v) {
	std::lock_guard<std::mutex> lock(mu_);
	master_ = v;
}

void AudioMixer::set_all_paused(bool paused) {
	std::lock_guard<std::mutex> lock(mu_);
	all_paused_ = paused;
}

void AudioMixer::stop_all() {
	std::lock_guard<std::mutex> lock(mu_);
	voices_.clear();
}

void AudioMixer::mix(float *out, int frames) {
	memset(out, 0, size_t(frames) * 2 * sizeof(float));
	std::lock_guard<std::mutex> lock(mu_);
	if (all_paused_) return;
	for (auto &vp : voices_) {
		Voice &v = *vp;
		if (v.done || v.paused) continue;
		double ratio = double(v.rate) / kOutputRate;
		v.fill(size_t(std::ceil(v.frac + frames * ratio)) + 2);
		float gl = v.params.volume * v.params.gain_l * master_;
		float gr = v.params.volume * v.params.gain_r * master_;
		int i = 0;
		for (; i < frames; ++i) {
			size_t k = size_t(v.frac);
			if (k >= v.buf_frames) break;
			const float *p = &v.buf[k * 2];
			float l = p[0], r = p[1];
			if (k + 1 < v.buf_frames) {
				float t = float(v.frac - double(k));
				l += (p[2] - l) * t;
				r += (p[3] - r) * t;
			}
			out[i * 2] += l * gl;
			out[i * 2 + 1] += r * gr;
			v.frac += ratio;
		}
		v.played += i;
		if (i < frames && v.eof) v.done = true;
	}
	voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const std::unique_ptr<Voice> &v) { return v->done; }),
	              voices_.end());
	for (int i = 0; i < frames * 2; ++i) out[i] = std::max(-1.0f, std::min(1.0f, out[i]));
}

}  // namespace ore
