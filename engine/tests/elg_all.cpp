// Decodes every ELG in every archive; writes a few samples as PNG.
#include <cstdio>
#include <string>

#include "res/elg.h"
#include "res/vfs.h"


#include "stb_image_write.h"

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: elg_all DATA_DIR OUT_DIR [sample-substring...]\n");
		return 2;
	}
	ore::Vfs vfs;
	std::string err;
	if (!vfs.mount_archives(argv[1], &err)) {
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	int ok = 0, bad = 0;
	for (const char *arcname : {"SYS", "CHR", "PIC"}) {
		const ore::LpkArchive *arc = vfs.archive(arcname);
		if (!arc) continue;
		for (const std::string &name : arc->names()) {
			if (name.size() < 4 || name.substr(name.size() - 4) != ".ELG") continue;
			std::vector<uint8_t> data;
			ore::ElgImage img;
			if (!arc->read(name, data) || !ore::decode_elg(data.data(), data.size(), img)) {
				printf("FAIL %s\\%s\n", arcname, name.c_str());
				++bad;
				continue;
			}
			++ok;
			for (int i = 3; i < argc; ++i) {
				if (name.find(argv[i]) == std::string::npos) continue;
				std::vector<uint8_t> rgba(img.bgra.size());
				for (size_t p = 0; p < img.bgra.size(); p += 4) {
					rgba[p] = img.bgra[p + 2];
					rgba[p + 1] = img.bgra[p + 1];
					rgba[p + 2] = img.bgra[p];
					rgba[p + 3] = img.bgra[p + 3];
				}
				std::string out = std::string(argv[2]) + "/" + arcname + "_" + name + ".png";
				stbi_write_png(out.c_str(), img.width, img.height, 4, rgba.data(), img.width * 4);
				printf("%s %dx%d bpp=%d type=%d off=%d,%d\n", out.c_str(), img.width, img.height,
				       img.bpp, img.type, img.offset_x, img.offset_y);
			}
		}
	}
	printf("ok=%d bad=%d\n", ok, bad);
	return bad ? 1 : 0;
}
