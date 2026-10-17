// Small string helpers shared by the engine.
#pragma once

#include <cstdint>
#include <string>

namespace ore {

inline std::string to_upper_ascii(std::string s) {
	for (char &c : s)
		if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
	return s;
}

// Normalizes a script path: upper case ASCII, '/' -> '\\'.
inline std::string normalize_path(const std::string &in) {
	std::string s = to_upper_ascii(in);
	for (char &c : s)
		if (c == '/') c = '\\';
	return s;
}

// Multibyte encoding of script text. The original engine treats SJIS lead
// bytes (0x81-0x9F, 0xE0-0xFC); the Chinese patch widens this to 0x81-0xFE.
enum class TextEncoding { ShiftJis, Gbk };

inline bool is_lead_byte(uint8_t c, TextEncoding enc) {
	if (enc == TextEncoding::Gbk) return c >= 0x81 && c <= 0xFE;
	return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

}  // namespace ore
