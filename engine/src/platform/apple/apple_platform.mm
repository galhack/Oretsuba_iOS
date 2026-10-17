#include "platform/apple/apple_platform.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <mach/mach_time.h>
#include <sys/stat.h>

#include <cmath>
#include <ctime>
#include <vector>

namespace ore {
namespace {

CFStringEncoding cf_encoding(TextEncoding enc) {
	return enc == TextEncoding::Gbk ? kCFStringEncodingGB_18030_2000 : kCFStringEncodingDOSJapanese;
}

bool face_is_serif(const std::string &face) {
	// "ＭＳ 明朝", "MS Mincho", "宋体" ...
	return face.find("\x96\xbe\x92\xa9") != std::string::npos ||  // 明朝 (cp932)
	       face.find("Mincho") != std::string::npos || face.find("MINCHO") != std::string::npos ||
	       face.find("\xcb\xce\xcc\xe5") != std::string::npos;    // 宋体 (gbk)
}

}  // namespace

ApplePlatform::ApplePlatform(std::string save_dir) : save_dir_(std::move(save_dir)) {
	mkdir(save_dir_.c_str(), 0755);
}

ApplePlatform::~ApplePlatform() {
	for (auto &kv : fonts_) CFRelease(static_cast<CTFontRef>(kv.second));
}

uint32_t ApplePlatform::decode_char(uint16_t code, TextEncoding enc) {
	std::lock_guard<std::mutex> lock(mu_);
	auto &cache = char_cache_[enc == TextEncoding::Gbk];
	auto it = cache.find(code);
	if (it != cache.end()) return it->second;
	uint8_t bytes[2];
	CFIndex n = 0;
	if (code > 0xFF) bytes[n++] = uint8_t(code >> 8);
	bytes[n++] = uint8_t(code);
	uint32_t cp = '?';
	CFStringRef s = CFStringCreateWithBytes(nullptr, bytes, n, cf_encoding(enc), false);
	if (s) {
		CFIndex len = CFStringGetLength(s);
		if (len >= 1) {
			UniChar c[2] = {0, 0};
			CFStringGetCharacters(s, CFRangeMake(0, std::min<CFIndex>(len, 2)), c);
			if (len == 2 && CFStringIsSurrogateHighCharacter(c[0]))
				cp = CFStringGetLongCharacterForSurrogatePair(c[0], c[1]);
			else
				cp = c[0];
		}
		CFRelease(s);
	}
	cache[code] = cp;
	return cp;
}

std::string ApplePlatform::to_utf8(const std::string &text, TextEncoding enc) {
	CFStringRef s = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(text.data()),
	                                        CFIndex(text.size()), cf_encoding(enc), false);
	if (!s) return text;
	CFIndex len = CFStringGetLength(s), need = 0;
	CFStringGetBytes(s, CFRangeMake(0, len), kCFStringEncodingUTF8, '?', false, nullptr, 0, &need);
	std::string out(size_t(need), '\0');
	CFStringGetBytes(s, CFRangeMake(0, len), kCFStringEncodingUTF8, '?', false,
	                 reinterpret_cast<UInt8 *>(&out[0]), need, nullptr);
	CFRelease(s);
	return out;
}

std::string ApplePlatform::from_utf8(const std::string &utf8, TextEncoding enc) {
	CFStringRef s = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(utf8.data()), CFIndex(utf8.size()),
	                                        kCFStringEncodingUTF8, false);
	if (!s) return std::string();
	CFIndex len = CFStringGetLength(s), need = 0;
	// characters the game encoding lacks become '?'
	CFStringGetBytes(s, CFRangeMake(0, len), cf_encoding(enc), '?', false, nullptr, 0, &need);
	std::string out(size_t(need), '\0');
	CFStringGetBytes(s, CFRangeMake(0, len), cf_encoding(enc), '?', false, reinterpret_cast<UInt8 *>(&out[0]), need,
	                 nullptr);
	CFRelease(s);
	return out;
}

const void *ApplePlatform::font_for(const std::string &face, int size, int weight) {
	bool serif = face_is_serif(face);
	bool bold = weight >= 600;
	std::string key = std::to_string(serif) + ":" + std::to_string(bold) + ":" + std::to_string(size) + ":" +
	                  std::to_string(chinese_fonts_);
	auto it = fonts_.find(key);
	if (it != fonts_.end()) return it->second;
	const char *name;
	if (chinese_fonts_)
		name = bold ? "PingFangSC-Semibold" : "PingFangSC-Regular";
	else if (serif)
		name = bold ? "HiraMinProN-W6" : "HiraMinProN-W3";
	else
		name = bold ? "HiraginoSans-W6" : "HiraginoSans-W3";
	CFStringRef cfname = CFStringCreateWithCString(nullptr, name, kCFStringEncodingUTF8);
	CTFontRef font = CTFontCreateWithName(cfname, CGFloat(size), nullptr);
	CFRelease(cfname);
	fonts_[key] = font;
	return font;
}

bool ApplePlatform::render_glyph(uint32_t codepoint, const std::string &face, int size, int weight,
                                 GlyphBitmap &out) {
	std::lock_guard<std::mutex> lock(mu_);
	CTFontRef font = static_cast<CTFontRef>(font_for(face, size, weight));
	UniChar chars[2];
	CFIndex nchars = 1;
	if (codepoint > 0xFFFF) {
		uint32_t v = codepoint - 0x10000;
		chars[0] = UniChar(0xD800 + (v >> 10));
		chars[1] = UniChar(0xDC00 + (v & 0x3FF));
		nchars = 2;
	} else {
		chars[0] = UniChar(codepoint);
	}
	CGGlyph glyphs[2] = {0, 0};
	CTFontRef use = font;
	bool fallback = false;
	if (!CTFontGetGlyphsForCharacters(font, chars, glyphs, nchars)) {
		CFStringRef str = CFStringCreateWithCharacters(nullptr, chars, nchars);
		use = CTFontCreateForString(font, str, CFRangeMake(0, nchars));
		CFRelease(str);
		fallback = true;
		if (!CTFontGetGlyphsForCharacters(use, chars, glyphs, nchars)) {
			CFRelease(use);
			return false;
		}
	}
	CGSize adv;
	CTFontGetAdvancesForGlyphs(use, kCTFontOrientationHorizontal, glyphs, &adv, 1);
	CGRect box = CTFontGetBoundingRectsForGlyphs(use, kCTFontOrientationHorizontal, glyphs, nullptr, 1);
	int x0 = int(std::floor(box.origin.x)) - 1;
	int y0 = int(std::floor(box.origin.y)) - 1;
	int x1 = int(std::ceil(box.origin.x + box.size.width)) + 1;
	int y1 = int(std::ceil(box.origin.y + box.size.height)) + 1;
	out.advance = int(std::lround(adv.width));
	out.width = std::max(0, x1 - x0);
	out.height = std::max(0, y1 - y0);
	out.left = x0;
	out.top = y1;  // pixels above the baseline
	out.alpha.assign(size_t(out.width) * out.height, 0);
	if (out.width && out.height) {
		CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
		CGContextRef ctx = CGBitmapContextCreate(out.alpha.data(), size_t(out.width), size_t(out.height), 8,
		                                         size_t(out.width), gray, kCGImageAlphaOnly);
		CGColorSpaceRelease(gray);
		CGContextSetShouldAntialias(ctx, true);
		CGContextSetShouldSmoothFonts(ctx, false);
		CGPoint pos = CGPointMake(-x0, -y0);
		CTFontDrawGlyphs(use, glyphs, &pos, 1, ctx);
		CGContextRelease(ctx);
	}
	if (fallback) CFRelease(use);
	return true;
}

void ApplePlatform::begin_dialog(const std::string &utf8_text, int kind) {
	if (!dialog_handler_) {
		dialog_result_ = 1;
		return;
	}
	dialog_result_ = -1;
	dialog_handler_(utf8_text, kind);
}

void ApplePlatform::begin_text_input(const std::string &utf8_initial) {
	if (!text_handler_) {
		text_result_ = 0;
		return;
	}
	text_result_ = -1;
	text_handler_(utf8_initial);
}

int ApplePlatform::poll_text_input(std::string &utf8_result) {
	if (text_result_ == 1) utf8_result = text_value_;
	return text_result_;
}

int64_t ApplePlatform::now_ms() {
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) mach_timebase_info(&tb);
	uint64_t t = mach_absolute_time();
	return int64_t(t * tb.numer / tb.denom / 1000000ull);
}

Platform::LocalTime ApplePlatform::local_time() {
	time_t t = time(nullptr);
	struct tm lt;
	localtime_r(&t, &lt);
	return LocalTime{lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, lt.tm_wday};
}

}  // namespace ore
