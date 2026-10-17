// Lucifen LPK archive reader.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ore {

struct LpkKey {
	uint32_t key1 = 0;
	uint32_t key2 = 0;
};

class LpkArchive {
public:
	// Opens path; base_name is the archive name used for key derivation
	// (e.g. "SYS"). file_key is the per-archive key from gameinit.sob, if any.
	static std::unique_ptr<LpkArchive> open(const std::string &path, const std::string &base_name,
	                                        const LpkKey *file_key, std::string *error);
	~LpkArchive();

	bool contains(const std::string &upper_name) const;
	bool read(const std::string &upper_name, std::vector<uint8_t> &out) const;
	std::vector<std::string> names() const;

	// Byte range of a stored (uncompressed, unencrypted) entry, for streaming
	// large movies straight from the archive. Returns false if unavailable.
	bool raw_range(const std::string &upper_name, uint64_t &offset, uint64_t &size) const;
	bool whole_crypt() const { return whole_; }

	// Everything needed to decode a stored (not LZSS-packed) entry piecewise.
	struct StreamInfo {
		std::string host_path;
		uint64_t offset = 0;      // of the stored bytes in the archive file
		uint64_t stored = 0;      // number of stored bytes
		std::vector<uint8_t> prefix;
		bool whole = false;       // per-byte xor/rotate over the stored bytes
		bool encrypted = false;   // first 0x100 stored bytes word-encrypted
		uint32_t key = 0;
	};
	bool stream_info(const std::string &upper_name, StreamInfo &info) const;

private:
	struct Entry {
		uint64_t offset = 0;
		uint32_t size = 0;
		uint32_t unpacked = 0;
	};
	LpkArchive() = default;
	bool parse_index(const std::vector<uint8_t> &index, std::string *error);
	void walk(const std::vector<uint8_t> &index, size_t pos, std::string &name, int depth);

	FILE *file_ = nullptr;
	std::string path_;
	uint32_t key1_ = 0;
	bool aligned_ = false, encrypted_ = false, packed_ = false, whole_ = false;
	int width_ = 2;
	size_t entries_off_ = 0;
	size_t entry_size_ = 9;
	std::vector<uint8_t> prefix_;
	std::map<std::string, Entry> entries_;
	bool walk_error_ = false;
};

// Decodes the content-wide obfuscation used by WholeCrypt archives in place.
void lpk_decrypt_whole(uint8_t *data, size_t n);
// Decrypts the word-encrypted head of an entry (at most 0x100 bytes).
void lpk_decrypt_head(uint8_t *data, size_t n, uint32_t key);

std::vector<uint8_t> lzss_decompress(const uint8_t *src, size_t n, size_t out_size);

}  // namespace ore
