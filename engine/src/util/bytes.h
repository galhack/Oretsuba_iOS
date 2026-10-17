// Little-endian byte reader/writer for file formats and save data.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ore {

class ByteReader {
public:
	explicit ByteReader(const std::vector<uint8_t> &d) : p_(d.data()), n_(d.size()) {}
	ByteReader(const uint8_t *p, size_t n) : p_(p), n_(n) {}

	bool bad() const { return bad_; }
	size_t pos() const { return pos_; }
	size_t remaining() const { return pos_ <= n_ ? n_ - pos_ : 0; }
	void seek(size_t pos) {
		if (pos > n_) bad_ = true;
		pos_ = pos;
	}
	void skip(size_t k) { seek(pos_ + k); }

	uint8_t u8() { return need(1) ? p_[pos_++] : 0; }
	uint16_t u16() {
		if (!need(2)) return 0;
		uint16_t v = uint16_t(p_[pos_] | (p_[pos_ + 1] << 8));
		pos_ += 2;
		return v;
	}
	uint32_t u32() {
		if (!need(4)) return 0;
		uint32_t v;
		memcpy(&v, p_ + pos_, 4);
		pos_ += 4;
		return v;
	}
	int32_t i32() { return int32_t(u32()); }
	std::string str(size_t n) {
		if (!need(n)) return std::string();
		std::string s(reinterpret_cast<const char *>(p_ + pos_), n);
		pos_ += n;
		return s;
	}
	std::string str_u32() { return str(u32()); }
	std::vector<uint8_t> bytes(size_t n) {
		if (!need(n)) return {};
		std::vector<uint8_t> v(p_ + pos_, p_ + pos_ + n);
		pos_ += n;
		return v;
	}

private:
	bool need(size_t k) {
		if (bad_ || pos_ + k > n_) {
			bad_ = true;
			return false;
		}
		return true;
	}
	const uint8_t *p_;
	size_t n_;
	size_t pos_ = 0;
	bool bad_ = false;
};

class ByteWriter {
public:
	void u8(uint8_t v) { d_.push_back(v); }
	void u16(uint16_t v) {
		d_.push_back(uint8_t(v));
		d_.push_back(uint8_t(v >> 8));
	}
	void u32(uint32_t v) {
		for (int i = 0; i < 4; ++i) d_.push_back(uint8_t(v >> (8 * i)));
	}
	void i32(int32_t v) { u32(uint32_t(v)); }
	void str(const std::string &s) {
		u32(uint32_t(s.size()));
		d_.insert(d_.end(), s.begin(), s.end());
	}
	void bytes(const std::vector<uint8_t> &b) { d_.insert(d_.end(), b.begin(), b.end()); }
	void blob(const std::vector<uint8_t> &b) {
		u32(uint32_t(b.size()));
		bytes(b);
	}
	std::vector<uint8_t> &data() { return d_; }
	std::vector<uint8_t> take() { return std::move(d_); }

private:
	std::vector<uint8_t> d_;
};

}  // namespace ore
