// MPEG-1 program stream movie player (op1/op2): pl_mpeg for demuxing and
// video, minimp3 for the audio track (MP2 in the original, MP3 in the
// Chinese patch's re-encodes). Audio goes to the mixer; video frames are
// paced by the audio clock.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "res/lpk.h"

namespace ore {

class AudioMixer;

// Random-access byte source for streaming media.
class ByteSource {
public:
	virtual ~ByteSource() = default;
	virtual uint64_t size() const = 0;
	virtual size_t read_at(uint64_t offset, uint8_t *buf, size_t n) = 0;

	static std::unique_ptr<ByteSource> from_file(const std::string &path);
	static std::unique_ptr<ByteSource> from_lpk(const LpkArchive::StreamInfo &info);
};

class MoviePlayer {
public:
	MoviePlayer();
	~MoviePlayer();

	bool open(std::unique_ptr<ByteSource> source, std::string *error);
	void start(AudioMixer *mixer, float volume);
	// Advances to wall time `now_ms`. Returns true when a new frame is ready
	// in frame() (BGRA, width()*height()).
	bool update(int64_t now_ms);
	bool finished() const { return finished_; }
	void stop();

	int width() const { return width_; }
	int height() const { return height_; }
	const std::vector<uint8_t> &frame() const { return bgra_; }

private:
	struct Impl;
	bool demux_more();
	void decode_audio(bool flush);
	double audio_clock() const;

	std::unique_ptr<Impl> impl_;
	int width_ = 0, height_ = 0;
	std::vector<uint8_t> bgra_;
	bool finished_ = false;
	int64_t start_ms_ = -1;
};

}  // namespace ore
