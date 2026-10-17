// Extras of the Chinese patch (中文三号补丁), reproduced from its hook DLL
// ORE_TUBA_CHS.dll (ORE_TUBA_Chinese/patch/patch.cpp):
//  * translator's notes ("TIPS"): when the shown message has a note, a TIPS
//    button appears at (692,427); clicking it shows the note over the game;
//  * the start-up thanks pictures shown before the game.
// The data is read at run time from the player's own copy of the DLL.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gfx/image.h"

namespace ore {

class Platform;

// Minimal PE resource reader: resource (type name, entry name) → bytes.
bool pe_resource(const std::vector<uint8_t> &pe, const std::string &type, const std::string &name,
                 std::vector<uint8_t> &out);

class ChsExtras {
public:
	bool load(const std::string &dll_path, Platform &platform);
	bool loaded() const { return loaded_; }

	// Start-up pictures; true while one is being shown.
	bool splash_active() const { return splash_index_ < splash_.size(); }
	void advance_splash() { ++splash_index_; }
	const Image *splash() const { return splash_active() ? splash_[splash_index_].get() : nullptr; }

	// Called with the record text (after the 3-byte header) of each message.
	void on_message(const std::string &record_gbk);
	// Pointer press in game coordinates; returns true when consumed.
	bool on_press(int x, int y);
	// Draws the button/panel over the frame; returns true if anything is drawn.
	bool draw(Image &frame);
	uint64_t generation() const { return gen_; }

private:
	void build_panel();
	Platform *platform_ = nullptr;
	bool loaded_ = false;
	std::map<std::string, std::string> tips_;  // UTF-8 message → UTF-8 note
	std::vector<std::unique_ptr<Image>> splash_;
	size_t splash_index_ = 0;
	std::unique_ptr<Image> button_;
	std::string current_;   // note of the current message ("" = none)
	bool panel_open_ = false;
	std::unique_ptr<Image> panel_;
	uint64_t gen_ = 1;
	std::string last_record_;
};

}  // namespace ore
