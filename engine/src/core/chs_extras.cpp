#include "core/chs_extras.h"

#include <cstdio>
#include <cstring>

#include "gfx/blend.h"
#include "platform/platform.h"
#include "stb_image.h"
#include "util/bytes.h"
#include "util/log.h"

namespace ore {
namespace {

constexpr int kButtonX = 692, kButtonY = 427;
constexpr int kPanelW = 790, kPanelH = 400;

std::unique_ptr<Image> decode_png(const std::vector<uint8_t> &png) {
	int w = 0, h = 0, n = 0;
	unsigned char *px = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
	if (!px) return nullptr;
	auto img = std::make_unique<Image>(w, h, 32);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const unsigned char *s = px + (size_t(y) * w + x) * 4;
			img->row32(y)[x] = uint32_t(s[2]) | (uint32_t(s[1]) << 8) | (uint32_t(s[0]) << 16) | (uint32_t(s[3]) << 24);
		}
	stbi_image_free(px);
	return img;
}

// Reads a UTF-16 resource directory name.
std::string res_name(const std::vector<uint8_t> &d, size_t rsrc, uint32_t off) {
	size_t p = rsrc + (off & 0x7fffffff);
	if (p + 2 > d.size()) return std::string();
	uint16_t n = uint16_t(d[p] | (d[p + 1] << 8));
	std::string s;
	for (uint16_t i = 0; i < n && p + 4 + i * 2 <= d.size(); ++i) s.push_back(char(d[p + 2 + i * 2]));
	return s;
}

uint32_t next_utf8(const std::string &s, size_t &i) {
	uint8_t c = uint8_t(s[i++]);
	if (c < 0x80) return c;
	int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
	uint32_t cp = c & (0x3F >> n);
	for (int k = 0; k < n && i < s.size(); ++k) cp = (cp << 6) | (uint8_t(s[i++]) & 0x3F);
	return cp;
}

}  // namespace

bool pe_resource(const std::vector<uint8_t> &d, const std::string &type, const std::string &name,
                 std::vector<uint8_t> &out) {
	ByteReader r(d);
	r.seek(0x3c);
	uint32_t pe = r.u32();
	r.seek(pe);
	if (r.u32() != 0x4550) return false;
	r.seek(pe + 6);
	uint16_t nsec = r.u16();
	r.seek(pe + 20);
	uint16_t opt_size = r.u16();
	size_t opt = pe + 24;
	r.seek(opt + 112);  // PE32 data directory 2 (resources)
	uint32_t rva = r.u32();
	size_t sec = opt + opt_size;
	size_t rsrc = 0;
	uint32_t sec_va = 0;
	for (int i = 0; i < nsec; ++i) {
		r.seek(sec + size_t(i) * 40 + 12);
		uint32_t va = r.u32();
		uint32_t raw_size = r.u32();
		uint32_t raw = r.u32();
		if (rva >= va && rva < va + raw_size) {
			rsrc = raw + (rva - va);
			sec_va = va;
			rsrc -= 0;
			(void)sec_va;
			break;
		}
	}
	if (!rsrc || r.bad()) return false;
	const size_t base = rsrc;
	auto entries = [&](size_t dir, std::vector<std::pair<uint32_t, uint32_t>> &v) {
		r.seek(dir + 12);
		uint16_t named = r.u16(), ids = r.u16();
		for (int i = 0; i < named + ids; ++i) {
			uint32_t a = r.u32(), b = r.u32();
			v.push_back({a, b});
		}
	};
	std::vector<std::pair<uint32_t, uint32_t>> types;
	entries(base, types);
	for (auto &t : types) {
		if (!(t.first & 0x80000000) || res_name(d, base, t.first) != type) continue;
		std::vector<std::pair<uint32_t, uint32_t>> names;
		entries(base + (t.second & 0x7fffffff), names);
		for (auto &n : names) {
			if (!(n.first & 0x80000000) || res_name(d, base, n.first) != name) continue;
			std::vector<std::pair<uint32_t, uint32_t>> langs;
			entries(base + (n.second & 0x7fffffff), langs);
			if (langs.empty()) return false;
			r.seek(base + langs[0].second);
			uint32_t data_rva = r.u32(), size = r.u32();
			size_t off = rsrc + (data_rva - rva);
			if (off + size > d.size()) return false;
			out.assign(d.begin() + long(off), d.begin() + long(off + size));
			return true;
		}
	}
	return false;
}

bool ChsExtras::load(const std::string &dll_path, Platform &platform) {
	platform_ = &platform;
	FILE *f = fopen(dll_path.c_str(), "rb");
	if (!f) return false;
	std::vector<uint8_t> d;
	fseek(f, 0, SEEK_END);
	d.resize(size_t(ftell(f)));
	fseek(f, 0, SEEK_SET);
	bool ok = fread(d.data(), 1, d.size(), f) == d.size();
	fclose(f);
	if (!ok) return false;
	std::vector<uint8_t> res;
	if (pe_resource(d, "TXT", "TIPSJSON", res)) {
		// {"key": "value", ...} with JSON string escapes
		std::string s(res.begin(), res.end());
		size_t i = 0;
		auto read_str = [&](std::string &out) -> bool {
			while (i < s.size() && s[i] != '"') ++i;
			if (i >= s.size()) return false;
			++i;
			out.clear();
			while (i < s.size() && s[i] != '"') {
				char c = s[i++];
				if (c == '\\' && i < s.size()) {
					char e = s[i++];
					switch (e) {
					case 'n': out.push_back('\n'); break;
					case 't': out.push_back('\t'); break;
					case 'r': break;
					case 'u': {
						uint32_t cp = uint32_t(strtoul(s.substr(i, 4).c_str(), nullptr, 16));
						i += 4;
						if (cp < 0x80) out.push_back(char(cp));
						else if (cp < 0x800) out.push_back(char(0xC0 | (cp >> 6))), out.push_back(char(0x80 | (cp & 0x3F)));
						else
							out.push_back(char(0xE0 | (cp >> 12))), out.push_back(char(0x80 | ((cp >> 6) & 0x3F))),
							    out.push_back(char(0x80 | (cp & 0x3F)));
						break;
					}
					default: out.push_back(e); break;
					}
				} else {
					out.push_back(c);
				}
			}
			++i;
			return true;
		};
		std::string k, v;
		while (read_str(k) && read_str(v)) tips_[k] = v;
	}
	if (pe_resource(d, "PNG", "TIPSPNG", res)) button_ = decode_png(res);
	for (const char *n : {"SHOWATFIRST", "SHOWATFIRST2", "SHOWATFIRST3"})
		if (pe_resource(d, "PNG", n, res))
			if (auto img = decode_png(res)) splash_.push_back(std::move(img));
	loaded_ = !tips_.empty() || !splash_.empty();
	log_info("chinese patch extras: %zu notes, %zu start pictures", tips_.size(), splash_.size());
	return loaded_;
}

void ChsExtras::on_message(const std::string &record_gbk) {
	if (record_gbk == last_record_) return;
	last_record_ = record_gbk;
	std::string key = platform_->to_utf8(record_gbk, TextEncoding::Gbk);
	auto it = tips_.find(key);
	std::string note = it == tips_.end() ? std::string() : it->second;
	if (note != current_) {
		current_ = note;
		panel_open_ = false;
		panel_.reset();
		++gen_;
	}
}

bool ChsExtras::on_press(int x, int y) {
	if (current_.empty() || !button_) return false;
	if (panel_open_) {
		panel_open_ = false;
		++gen_;
		return true;
	}
	if (x >= kButtonX && y >= kButtonY && x < kButtonX + button_->width() && y < kButtonY + button_->height()) {
		panel_open_ = true;
		if (!panel_) build_panel();
		++gen_;
		return true;
	}
	return false;
}

void ChsExtras::build_panel() {
	panel_ = std::make_unique<Image>(kPanelW, kPanelH, 32);
	panel_->fill(0xD0101828);
	const int size = 18, line_h = 28, margin = 18;
	int x = margin, y = margin;
	size_t i = 0;
	while (i < current_.size()) {
		uint32_t cp = next_utf8(current_, i);
		if (cp == '\n') {
			x = margin;
			y += line_h;
			continue;
		}
		GlyphBitmap g;
		if (!platform_->render_glyph(cp, "PingFang", size, 400, g)) continue;
		if (x + g.advance > kPanelW - margin) {
			x = margin;
			y += line_h;
		}
		if (y + line_h > kPanelH - margin) break;
		int base = y + 21;
		for (int gy = 0; gy < g.height; ++gy)
			for (int gx = 0; gx < g.width; ++gx) {
				int px = x + g.left + gx, py = base - g.top + gy;
				if (px < 0 || py < 0 || px >= kPanelW || py >= kPanelH) continue;
				uint32_t a = g.alpha[size_t(gy) * g.width + gx];
				if (!a) continue;
				uint32_t &d = panel_->row32(py)[px];
				uint32_t r = ((0xff - (d >> 16 & 0xff)) * a >> 8) + (d >> 16 & 0xff);
				uint32_t gg = ((0xff - (d >> 8 & 0xff)) * a >> 8) + (d >> 8 & 0xff);
				uint32_t b = ((0xff - (d & 0xff)) * a >> 8) + (d & 0xff);
				d = (d & 0xff000000u) | (r << 16) | (gg << 8) | b;
			}
		x += g.advance;
	}
}

bool ChsExtras::draw(Image &frame) {
	if (current_.empty() || !button_) return false;
	blit(frame, *button_, kButtonX, kButtonY, button_->rect(), 2, 255, false);
	if (panel_open_ && panel_) blit(frame, *panel_, 5, 5, panel_->rect(), 2, 255, false);
	return true;
}

}  // namespace ore
