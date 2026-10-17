// Platform implementation shared by the macOS test host and the iOS/Mac app.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "platform/platform.h"

namespace ore {

class ApplePlatform : public Platform {
public:
	explicit ApplePlatform(std::string save_dir);
	~ApplePlatform() override;

	uint32_t decode_char(uint16_t code, TextEncoding enc) override;
	std::string to_utf8(const std::string &text, TextEncoding enc) override;
	std::string from_utf8(const std::string &utf8, TextEncoding enc) override;
	bool render_glyph(uint32_t codepoint, const std::string &face, int size, int weight,
	                  GlyphBitmap &out) override;
	int64_t now_ms() override;
	std::string save_dir() override { return save_dir_; }
	LocalTime local_time() override;

	// Modal dialogs and text entry are shown by the app: the engine asks
	// through these handlers and the app answers with answer_*().
	using DialogHandler = std::function<void(const std::string &utf8, int kind)>;
	using TextInputHandler = std::function<void(const std::string &utf8_initial)>;
	void set_dialog_handler(DialogHandler h) { dialog_handler_ = std::move(h); }
	void set_text_input_handler(TextInputHandler h) { text_handler_ = std::move(h); }
	void answer_dialog(int result) { dialog_result_ = result; }
	void answer_text_input(bool ok, const std::string &utf8) {
		text_result_ = ok ? 1 : 0;
		text_value_ = utf8;
	}
	void begin_dialog(const std::string &utf8_text, int kind) override;
	int poll_dialog() override { return dialog_result_; }
	void begin_text_input(const std::string &utf8_initial) override;
	int poll_text_input(std::string &utf8_result) override;

	// Chinese text should use Chinese glyph shapes even for shared Han code points.
	void set_chinese_fonts(bool on) { chinese_fonts_ = on; }

private:
	const void *font_for(const std::string &face, int size, int weight);
	std::string save_dir_;
	DialogHandler dialog_handler_;
	TextInputHandler text_handler_;
	int dialog_result_ = 1;
	int text_result_ = 0;
	std::string text_value_;
	bool chinese_fonts_ = false;
	std::mutex mu_;
	std::map<std::string, const void *> fonts_;  // CTFontRef cache
	std::map<uint32_t, uint32_t> char_cache_[2];
};

}  // namespace ore
