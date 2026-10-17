#include "media/movie.h"

#include <cstdio>
#include <cstring>
#include <deque>

#include "audio/mixer.h"
#include "pl_mpeg.h"
#include "minimp3.h"

namespace ore {
namespace {

class FileSource : public ByteSource {
public:
	explicit FileSource(FILE *f) : f_(f) {
		fseeko(f_, 0, SEEK_END);
		size_ = uint64_t(ftello(f_));
	}
	~FileSource() override { fclose(f_); }
	uint64_t size() const override { return size_; }
	size_t read_at(uint64_t off, uint8_t *buf, size_t n) override {
		if (off >= size_) return 0;
		fseeko(f_, off_t(off), SEEK_SET);
		return fread(buf, 1, n, f_);
	}

private:
	FILE *f_;
	uint64_t size_ = 0;
};

// Logical entry bytes = prefix + decode(stored bytes).
class LpkSource : public ByteSource {
public:
	LpkSource(FILE *f, const LpkArchive::StreamInfo &info) : f_(f), info_(info) {
		size_t n = size_t(std::min<uint64_t>(info_.stored, 0x100));
		head_.resize(n);
		fseeko(f_, off_t(info_.offset), SEEK_SET);
		n = fread(head_.data(), 1, n, f_);
		head_.resize(n);
		if (info_.whole) lpk_decrypt_whole(head_.data(), head_.size());
		if (info_.encrypted) lpk_decrypt_head(head_.data(), head_.size(), info_.key);
	}
	~LpkSource() override { fclose(f_); }
	uint64_t size() const override { return info_.prefix.size() + info_.stored; }
	size_t read_at(uint64_t off, uint8_t *buf, size_t n) override {
		size_t done = 0;
		while (done < n && off < size()) {
			if (off < info_.prefix.size()) {
				buf[done++] = info_.prefix[size_t(off++)];
				continue;
			}
			uint64_t s = off - info_.prefix.size();
			if (s < head_.size()) {
				size_t k = std::min<size_t>(n - done, head_.size() - size_t(s));
				memcpy(buf + done, &head_[size_t(s)], k);
				done += k;
				off += k;
				continue;
			}
			size_t k = size_t(std::min<uint64_t>(n - done, info_.stored - s));
			fseeko(f_, off_t(info_.offset + s), SEEK_SET);
			k = fread(buf + done, 1, k, f_);
			if (!k) break;
			if (info_.whole) lpk_decrypt_whole(buf + done, k);
			done += k;
			off += k;
		}
		return done;
	}

private:
	FILE *f_;
	LpkArchive::StreamInfo info_;
	std::vector<uint8_t> head_;
};

}  // namespace

std::unique_ptr<ByteSource> ByteSource::from_file(const std::string &path) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return nullptr;
	return std::make_unique<FileSource>(f);
}

std::unique_ptr<ByteSource> ByteSource::from_lpk(const LpkArchive::StreamInfo &info) {
	FILE *f = fopen(info.host_path.c_str(), "rb");
	if (!f) return nullptr;
	return std::make_unique<LpkSource>(f, info);
}

struct MoviePlayer::Impl {
	std::unique_ptr<ByteSource> src;
	uint64_t read_pos = 0;
	plm_buffer_t *demux_buf = nullptr;
	plm_demux_t *demux = nullptr;
	plm_buffer_t *video_buf = nullptr;
	plm_video_t *video = nullptr;
	mp3dec_t mp3;
	std::vector<uint8_t> audio_bytes;  // elementary stream not yet decoded
	AudioMixer *mixer = nullptr;
	int voice = 0;
	int audio_rate = 0;
	float volume = 1.0f;
	bool demux_eof = false;
	bool have_audio = false;
	std::deque<std::vector<int16_t>> early_pcm;  // decoded before the voice opened
	double next_frame_time = 0.0;
	plm_frame_t *pending = nullptr;

	~Impl() {
		if (mixer && voice) mixer->stop(voice);
		if (video) plm_video_destroy(video);
		if (demux) plm_demux_destroy(demux);
	}
};

MoviePlayer::MoviePlayer() = default;
MoviePlayer::~MoviePlayer() = default;

bool MoviePlayer::open(std::unique_ptr<ByteSource> source, std::string *error) {
	impl_ = std::make_unique<Impl>();
	impl_->src = std::move(source);
	impl_->demux_buf = plm_buffer_create_with_capacity(512 * 1024);
	impl_->demux = plm_demux_create(impl_->demux_buf, 1);
	impl_->video_buf = plm_buffer_create_with_capacity(512 * 1024);
	impl_->video = plm_video_create_with_buffer(impl_->video_buf, 1);
	mp3dec_init(&impl_->mp3);
	// Feed until the video sequence header is known.
	for (int i = 0; i < 4096 && !plm_video_has_header(impl_->video); ++i) {
		if (!demux_more()) break;
	}
	if (!plm_video_has_header(impl_->video)) {
		if (error) *error = "movie: no MPEG-1 video header";
		return false;
	}
	width_ = plm_video_get_width(impl_->video);
	height_ = plm_video_get_height(impl_->video);
	bgra_.assign(size_t(width_) * height_ * 4, 0);
	return true;
}

// Reads more of the program stream and dispatches complete packets.
bool MoviePlayer::demux_more() {
	Impl &m = *impl_;
	plm_packet_t *pkt = plm_demux_decode(m.demux);
	if (!pkt) {
		if (m.demux_eof) return false;
		uint8_t chunk[64 * 1024];
		size_t n = m.src->read_at(m.read_pos, chunk, sizeof chunk);
		if (n == 0) {
			m.demux_eof = true;
			decode_audio(true);
			plm_buffer_signal_end(m.demux_buf);
			plm_buffer_signal_end(m.video_buf);
			return plm_demux_decode(m.demux) != nullptr;
		}
		m.read_pos += n;
		plm_buffer_write(m.demux_buf, chunk, n);
		return true;
	}
	if (pkt->type == PLM_DEMUX_PACKET_VIDEO_1) {
		plm_buffer_write(m.video_buf, pkt->data, pkt->length);
	} else if (pkt->type == PLM_DEMUX_PACKET_AUDIO_1) {
		m.audio_bytes.insert(m.audio_bytes.end(), pkt->data, pkt->data + pkt->length);
		// minimp3 needs several consecutive frames to trust a sync word; with
		// only a packet or two buffered it skips real frames as junk.
		if (m.audio_bytes.size() >= 16384) decode_audio(false);
	}
	return true;
}

void MoviePlayer::decode_audio(bool flush) {
	Impl &m = *impl_;
	size_t pos = 0;
	const size_t keep = flush ? 0 : 4096;
	mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	while (m.audio_bytes.size() - pos > keep) {
		mp3dec_frame_info_t info;
		int samples = mp3dec_decode_frame(&m.mp3, m.audio_bytes.data() + pos, int(m.audio_bytes.size() - pos), pcm, &info);
		if (info.frame_bytes == 0) break;
		pos += size_t(info.frame_bytes);
		if (samples <= 0) continue;
		std::vector<int16_t> st(size_t(samples) * 2);
		for (int i = 0; i < samples; ++i) {
			st[size_t(i) * 2] = pcm[i * info.channels];
			st[size_t(i) * 2 + 1] = pcm[i * info.channels + (info.channels > 1 ? 1 : 0)];
		}
		if (!m.audio_rate) m.audio_rate = info.hz;
		m.have_audio = true;
		if (m.voice)
			m.mixer->push_pcm(m.voice, st.data(), size_t(samples));
		else
			m.early_pcm.push_back(std::move(st));
	}
	m.audio_bytes.erase(m.audio_bytes.begin(), m.audio_bytes.begin() + long(pos));
}

void MoviePlayer::start(AudioMixer *mixer, float volume) {
	impl_->mixer = mixer;
	impl_->volume = volume;
	start_ms_ = -1;
}

double MoviePlayer::audio_clock() const {
	const Impl &m = *impl_;
	if (!m.voice || !m.audio_rate) return -1.0;
	return double(m.mixer->consumed_frames(m.voice)) / m.audio_rate;
}

bool MoviePlayer::update(int64_t now_ms) {
	if (finished_ || !impl_) return false;
	Impl &m = *impl_;
	if (start_ms_ < 0) start_ms_ = now_ms;
	// Open the audio voice once the stream's rate is known.
	if (!m.voice && m.mixer && m.audio_rate) {
		m.voice = m.mixer->open_stream(m.audio_rate, m.volume);
		for (auto &b : m.early_pcm) m.mixer->push_pcm(m.voice, b.data(), b.size() / 2);
		m.early_pcm.clear();
	}
	double clock = audio_clock();
	if (clock < 0) clock = double(now_ms - start_ms_) / 1000.0;
	// Keep roughly a second of audio queued.
	for (int guard = 0; guard < 256 && !m.demux_eof; ++guard) {
		bool need_audio = m.voice && m.mixer->queued_frames(m.voice) < size_t(m.audio_rate);
		if (!need_audio && m.pending) break;
		if (!need_audio && !m.pending) {
			m.pending = plm_video_decode(m.video);
			if (m.pending) break;
		}
		if (!demux_more()) break;
	}
	if (!m.pending) m.pending = plm_video_decode(m.video);
	bool shown = false;
	while (m.pending && m.next_frame_time <= clock) {
		plm_frame_to_bgra(m.pending, bgra_.data(), width_ * 4);
		for (size_t i = 3; i < bgra_.size(); i += 4) bgra_[i] = 0xFF;
		shown = true;
		m.next_frame_time += 1.0 / plm_video_get_framerate(m.video);
		m.pending = nullptr;
		// Drop frames when far behind.
		if (m.next_frame_time + 0.1 < clock) {
			m.pending = plm_video_decode(m.video);
			while (!m.pending && !m.demux_eof && demux_more()) m.pending = plm_video_decode(m.video);
		}
	}
	if (!m.pending && m.demux_eof) {
		m.pending = plm_video_decode(m.video);
		bool audio_left = m.voice && m.mixer->queued_frames(m.voice) > 0;
		if (!m.pending && !audio_left) finished_ = true;
	}
	return shown;
}

void MoviePlayer::stop() {
	finished_ = true;
	if (impl_ && impl_->mixer && impl_->voice) {
		impl_->mixer->stop(impl_->voice);
		impl_->voice = 0;
	}
}

}  // namespace ore
