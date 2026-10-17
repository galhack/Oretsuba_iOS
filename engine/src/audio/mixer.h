// Software audio mixer: streamed Ogg Vorbis voices (BGM), fully decoded PCM
// voices (SE, voice) and PCM stream voices (movies), mixed to interleaved
// stereo float. Thread-safe: the platform audio callback calls mix() from its
// own thread.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace ore {

// Interleaved stereo 16-bit PCM.
struct PcmBuffer {
	int rate = 44100;
	std::vector<int16_t> samples;  // 2 per frame
	size_t frames() const { return samples.size() / 2; }
};

// Decodes a whole Ogg Vorbis file to stereo PCM (mono is duplicated).
std::shared_ptr<PcmBuffer> decode_ogg(const std::vector<uint8_t> &data);
// Length of an Ogg Vorbis file in ms without decoding it.
int ogg_length_ms(const std::vector<uint8_t> &data);

class AudioMixer {
public:
	static constexpr int kOutputRate = 44100;

	struct PlayParams {
		float volume = 1.0f;  // linear gain
		float gain_l = 1.0f;  // pan gains
		float gain_r = 1.0f;
		bool loop = false;
	};

	AudioMixer();
	~AudioMixer();

	// Streams an Ogg file; when it ends and `next` is given, continues with
	// `next` (BGM intro + loop body). With loop set, the last file repeats.
	int play_ogg(std::shared_ptr<const std::vector<uint8_t>> data, std::shared_ptr<const std::vector<uint8_t>> next,
	             const PlayParams &params);
	int play_pcm(std::shared_ptr<const PcmBuffer> pcm, const PlayParams &params);
	// A voice fed with interleaved stereo s16 PCM via push_pcm (movies).
	int open_stream(int sample_rate, float volume);
	void push_pcm(int id, const int16_t *frames, size_t count);
	size_t queued_frames(int id);
	int64_t consumed_frames(int id);

	void stop(int id);
	void set_volume(int id, float volume);
	void set_pan(int id, float gain_l, float gain_r);
	void set_paused(int id, bool paused);
	bool playing(int id);
	// Playback position in ms since the voice started (loops wrap for PCM).
	int64_t position_ms(int id);
	// Position in source frames of the currently playing file.
	int64_t position_frames(int id);

	void set_master_volume(float v);
	void set_all_paused(bool paused);
	void stop_all();

	void mix(float *out, int frames);

private:
	struct Voice;
	Voice *find(int id);
	int add(std::unique_ptr<Voice> v);
	std::mutex mu_;
	std::vector<std::unique_ptr<Voice>> voices_;
	int next_id_ = 1;
	float master_ = 1.0f;
	bool all_paused_ = false;
};

// DirectSound pan (-10000..10000, hundredths of dB of attenuation applied to
// the opposite channel) to left/right gains.
void ds_pan_gains(int pan, float &left, float &right);

}  // namespace ore
