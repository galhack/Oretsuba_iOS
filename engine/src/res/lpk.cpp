// Lucifen LPK archive reader. Format notes follow GARbro's ArcLPK.cs (MIT).
#include "res/lpk.h"

#include <algorithm>
#include <cstring>

#include "util/text.h"

namespace ore {
namespace {

constexpr uint32_t kBaseKey1 = 0xA5B9AC6B;
constexpr uint32_t kBaseKey2 = 0x9A639DE5;
constexpr uint8_t kContentXor = 0x5D;
constexpr uint32_t kRotatePattern = 0x31746285;

uint32_t rotl(uint32_t v, unsigned n) {
	n &= 31;
	return n ? (v << n) | (v >> (32 - n)) : v;
}

uint32_t rotr(uint32_t v, unsigned n) {
	n &= 31;
	return n ? (v >> n) | (v << (32 - n)) : v;
}

uint32_t rd32(const uint8_t *p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

void wr32(uint8_t *p, uint32_t v) {
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

void decrypt_index(uint8_t *data, size_t length, uint32_t key) {
	uint32_t pattern = kRotatePattern;
	for (size_t i = 0; i + 4 <= length; i += 4) {
		wr32(data + i, rd32(data + i) ^ key);
		pattern = rotl(pattern, 4);
		key = rotr(key, pattern);
	}
}

void decrypt_entry(uint8_t *data, size_t length, uint32_t key) {
	uint32_t pattern = kRotatePattern;
	for (size_t i = 0; i + 4 <= length; i += 4) {
		wr32(data + i, rd32(data + i) ^ key);
		pattern = rotr(pattern, 4);
		key = rotl(key, pattern);
	}
}

}  // namespace

void lpk_decrypt_whole(uint8_t *data, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		uint8_t v = data[i] ^ kContentXor;
		data[i] = uint8_t((v >> 4) | (v << 4));
	}
}

void lpk_decrypt_head(uint8_t *data, size_t n, uint32_t key) {
	decrypt_entry(data, std::min<size_t>(n, 0x100) & ~size_t(3), key);
}

std::vector<uint8_t> lzss_decompress(const uint8_t *src, size_t n, size_t out_size) {
	std::vector<uint8_t> out;
	out.reserve(out_size);
	uint8_t win[4096] = {};
	unsigned wp = 0xFEE;
	size_t i = 0;
	while (out.size() < out_size && i < n) {
		uint8_t ctl = src[i++];
		for (int bit = 0; bit < 8 && out.size() < out_size && i < n; ++bit) {
			if (ctl & (1 << bit)) {
				uint8_t b = src[i++];
				out.push_back(b);
				win[wp] = b;
				wp = (wp + 1) & 0xFFF;
			} else {
				if (i + 1 >= n) {
					i = n;
					break;
				}
				unsigned lo = src[i], hi = src[i + 1];
				i += 2;
				unsigned off = lo | ((hi & 0xF0) << 4);
				unsigned cnt = (hi & 0x0F) + 3;
				for (unsigned k = 0; k < cnt; ++k) {
					uint8_t b = win[(off + k) & 0xFFF];
					out.push_back(b);
					win[wp] = b;
					wp = (wp + 1) & 0xFFF;
				}
			}
		}
	}
	out.resize(out_size);
	return out;
}

std::unique_ptr<LpkArchive> LpkArchive::open(const std::string &path, const std::string &base_name,
                                             const LpkKey *file_key, std::string *error) {
	std::unique_ptr<LpkArchive> arc(new LpkArchive());
	arc->path_ = path;
	arc->file_ = fopen(path.c_str(), "rb");
	if (!arc->file_) {
		if (error) *error = "cannot open " + path;
		return nullptr;
	}
	std::string name = to_upper_ascii(base_name);
	uint32_t k1 = kBaseKey1, k2 = kBaseKey2;
	for (size_t b = 0, e = name.size(); e-- > 0; ++b) {
		k1 ^= uint8_t(name[e]);
		k2 ^= uint8_t(name[b]);
		k1 = rotr(k1, 7);
		k2 = rotl(k2, 7);
	}
	if (file_key) {
		k1 ^= file_key->key1;
		k2 ^= file_key->key2;
	}
	arc->key1_ = k1;
	uint8_t hdr[8];
	if (fread(hdr, 1, 8, arc->file_) != 8 || memcmp(hdr, "LPK1", 4) != 0) {
		if (error) *error = "not an LPK1 archive: " + path;
		return nullptr;
	}
	uint32_t code = rd32(hdr + 4) ^ k2;
	uint32_t size = code & 0xFFFFFF;
	uint8_t flags = code >> 24;
	if (flags & 1) size = (size << 11) - 8;
	fseeko(arc->file_, 0, SEEK_END);
	uint64_t file_size = ftello(arc->file_);
	if (size < 5 || size > file_size || !(flags & 2)) {
		if (error) *error = "bad LPK index (wrong key?): " + path;
		return nullptr;
	}
	arc->aligned_ = flags & 1;
	arc->encrypted_ = flags & 4;
	arc->packed_ = flags & 8;
	arc->whole_ = (flags & 0x10) && name != "PATCH";
	std::vector<uint8_t> index(size);
	fseeko(arc->file_, 8, SEEK_SET);
	if (fread(index.data(), 1, size, arc->file_) != size) {
		if (error) *error = "short read: " + path;
		return nullptr;
	}
	decrypt_index(index.data(), size, k2);
	if (!arc->parse_index(index, error)) return nullptr;
	return arc;
}

LpkArchive::~LpkArchive() {
	if (file_) fclose(file_);
}

bool LpkArchive::parse_index(const std::vector<uint8_t> &ix, std::string *error) {
	uint32_t count = rd32(ix.data());
	size_t p = 4;
	size_t plen = ix[p++];
	prefix_.assign(ix.begin() + p, ix.begin() + p + plen);
	p += plen;
	width_ = ix[p++] ? 4 : 2;
	uint32_t letters = rd32(ix.data() + p);
	p += 4;
	entries_off_ = p + letters;
	entry_size_ = packed_ ? 13 : 9;
	if (count && (ix.size() - entries_off_) / entry_size_ < count)
		entry_size_ = (ix.size() - entries_off_) / count;
	std::string name;
	walk(ix, p, name, 0);
	if (walk_error_ || entries_.size() != count) {
		if (error) *error = "corrupt LPK index: " + path_;
		return false;
	}
	return true;
}

void LpkArchive::walk(const std::vector<uint8_t> &ix, size_t p, std::string &name, int depth) {
	if (p >= ix.size() || depth > 255) {
		walk_error_ = true;
		return;
	}
	unsigned n = ix[p++];
	for (unsigned i = 0; i < n && !walk_error_; ++i) {
		if (p + 1 + width_ > ix.size()) {
			walk_error_ = true;
			return;
		}
		uint8_t letter = ix[p++];
		int64_t next = width_ == 4 ? int32_t(rd32(&ix[p])) : (ix[p] | (ix[p + 1] << 8));
		p += width_;
		if (letter) {
			name.push_back(char(letter));
			walk(ix, p + next, name, depth + 1);
			name.pop_back();
		} else {
			size_t pos = entries_off_ + entry_size_ * size_t(next);
			if (pos + entry_size_ > ix.size()) {
				walk_error_ = true;
				return;
			}
			if (entry_size_ & 1) ++pos;
			Entry e;
			e.offset = rd32(&ix[pos]);
			if (aligned_) e.offset <<= 11;
			e.size = rd32(&ix[pos + 4]);
			if (packed_) e.unpacked = rd32(&ix[pos + 8]);
			entries_[to_upper_ascii(name)] = e;
		}
	}
}

bool LpkArchive::contains(const std::string &upper_name) const {
	return entries_.count(upper_name) != 0;
}

std::vector<std::string> LpkArchive::names() const {
	std::vector<std::string> out;
	for (auto &kv : entries_) out.push_back(kv.first);
	return out;
}

bool LpkArchive::read(const std::string &upper_name, std::vector<uint8_t> &out) const {
	auto it = entries_.find(upper_name);
	if (it == entries_.end()) return false;
	const Entry &e = it->second;
	// The recorded size includes the common prefix stripped from every entry.
	size_t stored = e.size >= prefix_.size() ? e.size - prefix_.size() : e.size;
	std::vector<uint8_t> raw(stored);
	fseeko(file_, e.offset, SEEK_SET);
	if (fread(raw.data(), 1, stored, file_) != stored) return false;
	std::vector<uint8_t> data = e.unpacked ? lzss_decompress(raw.data(), raw.size(), e.unpacked)
	                                       : std::move(raw);
	if (whole_) lpk_decrypt_whole(data.data(), data.size());
	if (encrypted_) {
		size_t n = std::min<size_t>(data.size(), 0x100) & ~size_t(3);
		decrypt_entry(data.data(), n, key1_);
	}
	out.clear();
	out.reserve(prefix_.size() + data.size());
	out.insert(out.end(), prefix_.begin(), prefix_.end());
	out.insert(out.end(), data.begin(), data.end());
	return true;
}

bool LpkArchive::raw_range(const std::string &upper_name, uint64_t &offset, uint64_t &size) const {
	auto it = entries_.find(upper_name);
	if (it == entries_.end() || it->second.unpacked) return false;
	offset = it->second.offset;
	size = it->second.size >= prefix_.size() ? it->second.size - prefix_.size() : it->second.size;
	return true;
}

}  // namespace ore

namespace ore {

bool LpkArchive::stream_info(const std::string &upper_name, StreamInfo &info) const {
	auto it = entries_.find(upper_name);
	if (it == entries_.end() || it->second.unpacked) return false;
	info.host_path = path_;
	info.offset = it->second.offset;
	info.stored = it->second.size >= prefix_.size() ? it->second.size - prefix_.size() : it->second.size;
	info.prefix = prefix_;
	info.whole = whole_;
	info.encrypted = encrypted_;
	info.key = key1_;
	return true;
}

}  // namespace ore
