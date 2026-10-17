// Private state of the text system shared by text_system.cpp and the natives.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "text/lag.h"
#include "text/message.h"
#include "text/text_system.h"

namespace ore {

struct TextSystem::Impl {
	explicit Impl(Engine &e) : mm(e) {}
	MessageManager mm;
	std::vector<uint8_t> opt_snapshot;  // N_c dirty tracking
	std::vector<std::unique_ptr<LagData>> lags;      // DAT_00503520
	std::vector<std::unique_ptr<AnimData>> anims;    // DAT_00503508
	std::vector<std::string> font_list;              // N_bd
	std::vector<uint32_t> font_list_mem;             // VM copies for N_bf
};

}  // namespace ore
