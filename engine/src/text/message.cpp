// Message manager (docs/re/40_text.md §2-4).
#include "text/message.h"

#include <algorithm>
#include <cstring>

#include "core/engine.h"
#include "util/bytes.h"

namespace ore {

MessageManager::MessageManager(Engine &e) : e_(e) {}

// ---------------------------------------------------------------- record

int MessageManager::set_message(uint32_t rec) {
	if (!enabled) return -1;
	VmMemory &m = e_.vm.mem();
	const uint8_t *h = m.host(rec, 3);
	type = h[0];
	line = uint16_t(h[1] | h[2] << 8);
	read = is_read(type, line);
	// FUN_00455aa0: '#' name '$', then any run of '$' / '&'
	const char *p = m.cstr(rec + 3);
	name.clear();
	if (*p == '#') {
		const char *s = ++p;
		while (*p && *p != '$') ++p;
		name.assign(s, size_t(p - s));
	}
	while (*p == '$' || *p == '&') ++p;
	body = p;
	if (!shown_since_choices) attach_choices();
	push_backlog();
	pos = 0;
	bookmark = 0;
	shown_since_choices = true;
	return 0;
}

void MessageManager::clear() {
	name.clear();
	body.clear();
	pos = 0;
	read = false;
	clear_choices();
	shown_since_choices = true;
}

std::string MessageManager::name_segment(int idx) const {
	if (idx <= 0) return name;
	size_t s = 0;
	for (int k = 1; k < idx; ++k) {
		size_t b = name.find('\\', s);
		if (b == std::string::npos) return std::string();
		s = b + 1;
	}
	size_t b = name.find('\\', s);
	return name.substr(s, b == std::string::npos ? std::string::npos : b - s);
}

bool MessageManager::has_name_segment(int idx) const {
	if (idx <= 1) return !name.empty() || idx <= 0;
	size_t s = 0;
	for (int k = 1; k < idx; ++k) {
		size_t b = name.find('\\', s);
		if (b == std::string::npos) return false;
		s = b + 1;
	}
	return true;
}

namespace {

// Parses the number of a <N> tag (FUN_004bf190): [-]dec, 0x hex, 0b bin;
// spaces, tabs, ',', '(' and ')' are skipped.
bool parse_tag_number(const std::string &b, size_t &i, int &value) {
	auto skip = [&]() {
		while (i < b.size() && (b[i] == ' ' || b[i] == '\t' || b[i] == ',' || b[i] == '(' || b[i] == ')')) ++i;
	};
	skip();
	bool neg = false;
	if (i < b.size() && b[i] == '-') neg = true, ++i;
	int base = 10;
	if (i + 1 < b.size() && b[i] == '0' && (b[i + 1] == 'x' || b[i + 1] == 'X')) base = 16, i += 2;
	else if (i + 1 < b.size() && b[i] == '0' && (b[i + 1] == 'b' || b[i + 1] == 'B')) base = 2, i += 2;
	long v = 0;
	bool any = false;
	while (i < b.size()) {
		char c = b[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
		if (d >= base) break;
		v = v * base + d;
		any = true;
		++i;
	}
	skip();
	value = int(neg ? -v : v);
	return any;
}

}  // namespace

int MessageManager::step(MsgToken &t) {
	t = MsgToken();
	const std::string &b = body;
	auto end_code = [&](int code) {
		t.code = code;
		if (!read) mark_read(type, line);
		return code;
	};
	if (pos >= b.size()) return end_code(kMsgEnd);
	uint8_t c = uint8_t(b[pos]);
	if (c == '$') {
		++pos;
		if (pos >= b.size()) return end_code(kMsgWaitEnd);
		if (b[pos] == '&') return end_code(kMsgWaitPage);
		t.code = kMsgWait;
		return kMsgWait;
	}
	if (c == '&') {
		++pos;
		return end_code(kMsgClear);
	}
	if (c == '<') {
		if (pos + 1 < b.size() && b[pos + 1] == '<') {
			pos += 2;
			t.code = kMsgChar;
			t.ch = '<';
			return kMsgChar;
		}
		size_t i = pos + 1;
		while (i < b.size() && (b[i] == ' ' || b[i] == '\t')) ++i;
		int code = -1;
		if (i < b.size() && b[i] == '"') {
			size_t q = b.find('"', i + 1);
			if (q != std::string::npos) {
				t.str = b.substr(i + 1, q - i - 1);
				i = q + 1;
				while (i < b.size() && b[i] != '>') ++i;
				code = kMsgString;
			}
		} else if (parse_tag_number(b, i, t.value) && i < b.size() && b[i] == '>') {
			code = kMsgSize;
		}
		if (code >= 0 && i < b.size() && b[i] == '>') {
			pos = i + 1;
			if (pos < b.size() && b[pos] == '\\') ++pos;  // a '\' right after a tag is swallowed
			if (code == kMsgString) last_string = t.str;
			else last_size = t.value;
			t.code = code;
			return code;
		}
		// malformed: plain '<'
	}
	if (c == '@' && pos + 1 < b.size() && b[pos + 1] == '@') {
		pos += 2;
		t.code = kMsgChar;
		t.ch = '@';
		return kMsgChar;
	}
	if (is_lead_byte(c, e_.encoding) && pos + 1 < b.size()) {
		t.ch = uint16_t(c << 8 | uint8_t(b[pos + 1]));
		pos += 2;
	} else {
		t.ch = c;
		++pos;
	}
	t.code = kMsgChar;
	return kMsgChar;
}

void MessageManager::skip_to_end() {
	pos = body.size();
	if (!read) mark_read(type, line);
}

std::string MessageManager::plain_text(bool crlf) const {
	return message_plain_text(body, e_.encoding, crlf);
}

std::string message_plain_text(const std::string &b, TextEncoding enc, bool crlf) {
	std::string out;
	size_t i = 0;
	while (i < b.size()) {
		uint8_t c = uint8_t(b[i]);
		if (c == '$' || c == '&') break;
		if (c == '<') {
			if (i + 1 < b.size() && b[i + 1] == '<') {
				out += '<';
				i += 2;
				continue;
			}
			size_t q = b.find('>', i);
			if (q == std::string::npos) break;
			i = q + 1;
			if (i < b.size() && b[i] == '\\') ++i;
			continue;
		}
		if (c == '@') {
			if (i + 1 < b.size() && b[i + 1] == '@') {
				out += '@';
				i += 2;
				continue;
			}
			++i;
			while (i < b.size() && b[i] >= '0' && b[i] <= '9') ++i;
			continue;
		}
		if (c == '[' && i + 1 < b.size()) {
			out += b[i + 1];
			i += 2;
			continue;
		}
		if (c == '\\') {
			if (crlf) out += "\r\n";
			++i;
			continue;
		}
		if (is_lead_byte(c, enc) && i + 1 < b.size()) {
			out.append(b, i, 2);
			i += 2;
			continue;
		}
		out += char(c);
		++i;
	}
	return out;
}

// ---------------------------------------------------------------- read flags

ScriptInitEntry *MessageManager::flags_entry(int rec_type) {
	auto &tab = e_.vm.init_table();
	if (tab.empty()) return nullptr;
	if (rec_type != 0) return &tab.back();
	const ScriptBlob *s = e_.vm.current_script();
	if (!s || s->init_index < 0 || size_t(s->init_index) >= tab.size()) return nullptr;
	return &tab[size_t(s->init_index)];
}

bool MessageManager::is_read(int rec_type, int line_no) {
	ScriptInitEntry *en = flags_entry(rec_type);
	if (!en || line_no < 0 || size_t(line_no >> 3) >= en->bits_a.size()) return false;
	return en->bits_a[size_t(line_no >> 3)] & (1 << (line_no & 7));
}

void MessageManager::mark_read(int rec_type, int line_no) {
	ScriptInitEntry *en = flags_entry(rec_type);
	if (en && line_no >= 0 && size_t(line_no >> 3) < en->bits_a.size())
		en->bits_a[size_t(line_no >> 3)] |= uint8_t(1 << (line_no & 7));
}

// ---------------------------------------------------------------- backlog

void MessageManager::set_capacity(int cap) {
	ring.clear();
	capacity = std::max(0, cap);
	ring.resize(size_t(capacity));
	write = used = view = 0;
}

void MessageManager::push_backlog() {
	if (!log_enabled || capacity <= 0) return;
	auto en = std::make_unique<Entry>();
	en->name = name;
	en->body = body;
	en->type = type;
	en->line = line;
	ring[size_t(write)] = std::move(en);
	write = (write + 1) % capacity;
	used = std::min(used + 1, capacity);
	view = used - 1;
}

MessageManager::Entry *MessageManager::backlog_at(int k) {
	if (k < 0 || k >= used || capacity <= 0) return nullptr;
	int idx = ((write - used + k) % capacity + capacity) % capacity;
	return ring[size_t(idx)].get();
}

int MessageManager::load_backlog(int k) {
	Entry *en = backlog_at(k);
	if (!en) return -1;
	name = en->name;
	body = en->body;
	type = en->type;
	line = en->line;
	pos = 0;
	read = is_read(type, line);
	clear_choices();
	if (en->has_choices) {
		choices = en->choices;
		selected = en->selected;
		choice_type = en->choice_type;
		choice_base = en->choice_base;
		choice_bits = uint8_t(choices.size());
	}
	shown_since_choices = true;
	view = k;
	return 0;
}

void MessageManager::attach_choices() {
	if (choices.empty() || !log_enabled || used == 0) return;
	Entry *en = backlog_at(used - 1);
	if (!en || en->has_choices) return;
	en->has_choices = true;
	en->choices = choices;
	en->selected = selected;
	en->choice_type = choice_type;
	en->choice_base = choice_base;
}

std::vector<uint8_t> MessageManager::save_log() const {
	ByteWriter w;
	w.u32(0x474f4c42);  // "BLOG"
	std::vector<const Entry *> list;
	for (int k = 0; k < used; ++k) {
		int idx = ((write - used + k) % capacity + capacity) % capacity;
		if (ring[size_t(idx)]) list.push_back(ring[size_t(idx)].get());
	}
	w.u32(uint32_t(list.size()));
	for (const Entry *en : list) {
		w.str(en->name);
		w.str(en->body);
		w.u8(en->type);
		w.u16(en->line);
		w.u8(en->has_choices);
		w.u8(en->selected);
		w.u8(en->choice_type);
		w.u16(en->choice_base);
		w.u32(uint32_t(en->choices.size()));
		for (const std::string &c : en->choices) w.str(c);
	}
	return w.take();
}

void MessageManager::load_log(const std::vector<uint8_t> &data) {
	ByteReader r(data);
	if (r.u32() != 0x474f4c42) return;
	uint32_t n = r.u32();
	set_capacity(capacity);
	for (uint32_t i = 0; i < n && !r.bad(); ++i) {
		auto en = std::make_unique<Entry>();
		en->name = r.str_u32();
		en->body = r.str_u32();
		en->type = r.u8();
		en->line = r.u16();
		en->has_choices = r.u8();
		en->selected = r.u8();
		en->choice_type = r.u8();
		en->choice_base = r.u16();
		uint32_t nc = r.u32();
		for (uint32_t k = 0; k < nc && k < 256 && !r.bad(); ++k) en->choices.push_back(r.str_u32());
		if (r.bad() || capacity <= 0) break;
		ring[size_t(write)] = std::move(en);
		write = (write + 1) % capacity;
		used = std::min(used + 1, capacity);
	}
	view = std::max(0, used - 1);
}

// ---------------------------------------------------------------- choices

void MessageManager::clear_choices() {
	choices.clear();
	for (uint32_t a : choice_mem_)
		if (a) e_.vm.mem().free(a);
	choice_mem_.clear();
	selected = 0xff;
	choice_type = 2;
	choice_base = 0;
	choice_bits = 0;
}

int MessageManager::set_choices(uint32_t rec) {
	if (!choices_enabled) return -1;
	VmMemory &m = e_.vm.mem();
	const uint8_t *h = m.host(rec, 5);
	if (log_enabled && used > 0) {
		Entry *en = backlog_at(used - 1);
		if (en && en->has_choices) return -2;
	}
	clear_choices();
	choice_type = h[0];
	choice_base = uint16_t(h[1] | h[2] << 8);
	choice_bits = h[3];
	int n = std::min<int>(h[3], choice_capacity);
	uint32_t p = rec + 5;
	for (int i = 0; i < n; ++i) {
		uint8_t sz = m.host(p, 2)[0];
		choices.push_back(m.cstr(p + 2));
		if (sz < 3) break;
		p += sz;
	}
	shown_since_choices = false;
	return 0;
}

int MessageManager::select_choice(int i) {
	if (i < 0 || i >= int(choices.size())) return 0;
	selected = uint8_t(i);
	if (shown_since_choices && log_enabled && used > 0) {
		Entry *en = backlog_at(used - 1);
		if (en && en->has_choices) en->selected = uint8_t(i);
	}
	if (choice_type != 2 && i < choice_bits) {
		ScriptInitEntry *en = flags_entry(choice_type);
		int bit = choice_base + i;
		if (en && size_t(bit >> 3) < en->bits_b.size()) en->bits_b[size_t(bit >> 3)] |= uint8_t(1 << (bit & 7));
	}
	return 1;
}

bool MessageManager::choice_chosen(int i) {
	if (choice_type == 2 || i < 0) return false;
	ScriptInitEntry *en = flags_entry(choice_type);
	int bit = choice_base + i;
	if (!en || size_t(bit >> 3) >= en->bits_b.size()) return false;
	return en->bits_b[size_t(bit >> 3)] & (1 << (bit & 7));
}

uint32_t MessageManager::choice_ptr(int i) {
	if (i < 0 || i >= int(choices.size())) return 0;
	if (choice_mem_.size() < choices.size()) choice_mem_.resize(choices.size(), 0);
	uint32_t &a = choice_mem_[size_t(i)];
	if (!a) a = e_.vm.mem().alloc_string(choices[size_t(i)], "choice");
	return a;
}

}  // namespace ore
