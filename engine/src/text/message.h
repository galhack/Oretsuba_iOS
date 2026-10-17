// Message manager (docs/re/40_text.md §2-4, DAT_00503258): the current text
// record, its tag scanner, read flags, the backlog ring and the choice list.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "util/text.h"

namespace ore {

class Engine;
struct ScriptInitEntry;

// One step of the body scanner (FUN_00456540).
enum MsgCode {
	kMsgChar = 0,      // ch = character code ('\\' = line break)
	kMsgWait = 1,      // '$' + other: wait for click, same page
	kMsgWaitPage = 2,  // "$&": wait for click, then new page
	kMsgWaitEnd = 3,   // "$\0": wait for click (end of message)
	kMsgClear = 4,     // '&': new page
	kMsgEnd = 5,       // NUL
	kMsgString = 6,    // <"str">
	kMsgSize = 7,      // <N>
	kMsgBlock = 8,     // [\0 ...
};

struct MsgToken {
	int code = kMsgEnd;
	uint16_t ch = 0;
	int value = 0;
	std::string str;
};

// FUN_00433000 on any body: tags and @nnn dropped, '\\' -> CRLF when crlf,
// stops at NUL / '$' / '&'.
std::string message_plain_text(const std::string &body, TextEncoding enc, bool crlf);

class MessageManager {
public:
	explicit MessageManager(Engine &e);

	// ---- current record
	bool enabled = true;      // +0x20 (N_1cf)
	std::string name;         // raw "name\voice"
	std::string body;
	size_t pos = 0;           // +0x58 read position in body
	bool read = false;        // +0x1c
	uint8_t type = 0;         // +0x1d
	uint16_t line = 0;        // +0x1e
	int last_size = 0;        // +0x60 (N_1d9)
	std::string last_string;  // +0x5c (N_1d8)
	size_t bookmark = 0;      // N_1e5/N_1e6

	int set_message(uint32_t rec_addr);  // N_1ce
	int step(MsgToken &t);               // FUN_00456540
	void skip_to_end();                  // FUN_00456a90(-1)
	void rewind() { pos = 0; }
	// FUN_004561f0: 0 whole, 1 display name, 2 voice id ...; empty if absent.
	std::string name_segment(int idx) const;
	bool has_name_segment(int idx) const;
	// FUN_00433000: plain text of the body (tags dropped, '\' -> CRLF when crlf).
	std::string plain_text(bool crlf) const;
	void clear();  // FUN_00455430

	// ---- read flags (vm.init_table bitsets)
	ScriptInitEntry *flags_entry(int rec_type);
	bool is_read(int rec_type, int line_no);
	void mark_read(int rec_type, int line_no);

	// ---- backlog (history.sob)
	struct Entry {
		std::string name, body;
		std::vector<std::string> choices;
		bool has_choices = false;
		uint8_t selected = 0xff;
		uint8_t type = 0;
		uint16_t line = 0;
		uint8_t choice_type = 2;
		uint16_t choice_base = 0;
	};
	bool log_enabled = false;  // +0x50 (N_1cd)
	int capacity = 0;          // +0x44
	int write = 0;             // +0x48
	int used = 0;              // +0x4c
	int view = 0;              // +0x54
	std::vector<std::unique_ptr<Entry>> ring;
	void set_capacity(int cap);  // FUN_00455520
	void push_backlog();         // FUN_00455590
	Entry *backlog_at(int k);    // 0 = oldest
	int load_backlog(int k);     // FUN_00456380, 0 ok / -1
	std::vector<uint8_t> save_log() const;
	void load_log(const std::vector<uint8_t> &data);

	// ---- choices
	bool choices_enabled = true;  // +0x3e
	int choice_capacity = 8;      // +0x24
	std::vector<std::string> choices;
	bool shown_since_choices = true;  // +0x38
	uint8_t choice_type = 2;      // +0x39
	uint16_t choice_base = 0;     // +0x3a
	uint8_t choice_bits = 0;      // +0x3c
	uint8_t selected = 0xff;      // +0x3d
	int set_choices(uint32_t rec_addr);  // N_1eb
	void clear_choices();                // FUN_00455dc0
	int select_choice(int i);            // N_1f5
	bool choice_chosen(int i);           // N_1f7
	uint32_t choice_ptr(int i);          // N_1f4: VM copy of choice i

private:
	void attach_choices();  // FUN_004557f0
	Engine &e_;
	std::vector<uint32_t> choice_mem_;  // VM buffers handed to scripts
};

}  // namespace ore
