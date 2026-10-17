// Decodes a movie as fast as possible, timing it and dumping a few frames.
#include <chrono>
#include <cstdio>
#include <string>

#include "audio/mixer.h"
#include "media/movie.h"
#include "res/vfs.h"
#include "stb_image_write.h"

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: movie_test DATA_DIR MOVIE OUT_DIR\n");
		return 2;
	}
	std::unique_ptr<ore::ByteSource> src;
	std::string arg = argv[2];
	if (arg.find('\\') != std::string::npos) {
		ore::Vfs vfs;
		std::string err;
		if (!vfs.mount_archives(argv[1], &err)) return 1;
		ore::Vfs::Location loc;
		vfs.locate(arg, loc);
		ore::LpkArchive::StreamInfo info;
		loc.archive->stream_info(loc.entry, info);
		src = ore::ByteSource::from_lpk(info);
	} else {
		src = ore::ByteSource::from_file(arg);
	}
	// The mixer must outlive the player, which stops its voice on destruction.
	ore::AudioMixer mixer;
	ore::MoviePlayer mp;
	std::string err;
	if (!src || !mp.open(std::move(src), &err)) {
		fprintf(stderr, "open: %s\n", err.c_str());
		return 1;
	}
	mp.start(&mixer, 1.0f);
	printf("%dx%d\n", mp.width(), mp.height());
	auto t0 = std::chrono::steady_clock::now();
	int frames = 0;
	float buf[2 * 1470];
	for (int64_t ms = 0; !mp.finished() && ms < 200000; ms += 33) {
		if (mp.update(ms)) {
			++frames;
			if (frames == 300 || frames == 1500) {
				std::vector<uint8_t> rgba(mp.frame());
				for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
				std::string out = std::string(argv[3]) + "/movie_" + std::to_string(frames) + ".png";
				stbi_write_png(out.c_str(), mp.width(), mp.height(), 4, rgba.data(), mp.width() * 4);
			}
		}
		mixer.mix(buf, 1455);  // ~33 ms of audio
	}
	double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	printf("frames=%d in %.2fs (%.1f fps decode)\n", frames, s, frames / s);
	return mp.finished() && frames > 0 ? 0 : 1;
}
