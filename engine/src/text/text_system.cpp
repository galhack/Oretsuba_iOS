// Text system: owns the message manager; save support (docs/re/40_text.md §3,
// docs/re/10_lifecycle_system.md §6).
#include "text/text_system.h"

#include <cstring>

#include "core/engine.h"
#include "text/text_impl.h"

namespace ore {

TextSystem::TextSystem(Engine &e) : engine(e), impl(std::make_unique<Impl>(e)) {
	int n = e.opt8(0xe5a);
	impl->mm.set_capacity(n ? n + 1 : 0);
}

TextSystem::~TextSystem() = default;

MessageManager &TextSystem::msg() {
	return impl->mm;
}

std::vector<uint8_t> TextSystem::save_backlog() {
	return impl->mm.save_log();
}

void TextSystem::load_backlog(const std::vector<uint8_t> &data) {
	impl->mm.load_log(data);
}

void TextSystem::clear_message_state() {
	impl->mm.clear();
}

std::string TextSystem::preview_text(size_t max) {
	MessageManager &mm = impl->mm;
	std::string nm, body;
	if (MessageManager::Entry *en = mm.backlog_at(mm.used - 1)) {
		nm = en->name;
		body = en->body;
	} else {
		nm = mm.name;
		body = mm.body;
	}
	size_t bs = nm.find('\\');
	if (bs != std::string::npos) nm.resize(bs);
	bool gbk = engine.encoding == TextEncoding::Gbk;
	std::string out;
	if (!nm.empty()) out = (gbk ? "\xa1\xbe" : "\x81\x79") + nm + (gbk ? "\xa1\xbf" : "\x81\x7a") + "\n";
	out += message_plain_text(body, engine.encoding, false);
	// cut on a character boundary
	size_t i = 0, keep = 0;
	while (i < out.size()) {
		size_t n = is_lead_byte(uint8_t(out[i]), engine.encoding) && i + 1 < out.size() ? 2 : 1;
		if (i + n > max) break;
		i += n;
		keep = i;
	}
	out.resize(keep);
	return out;
}

std::string TextSystem::reflow(const std::string &text, int cols, int lines) {
	std::string out;
	int nlines = 0, col = 0;
	auto brk = [&]() {
		if (++nlines >= lines) return false;
		out += '\n';
		col = 0;
		return true;
	};
	for (size_t i = 0; i < text.size();) {
		uint8_t c = uint8_t(text[i]);
		if (c == '\r') {
			++i;
			continue;
		}
		if (c == '\n') {
			if (!brk()) break;
			++i;
			continue;
		}
		size_t n = is_lead_byte(c, engine.encoding) && i + 1 < text.size() ? 2 : 1;
		if (col + int(n) > cols && !brk()) break;
		out.append(text, i, n);
		col += int(n);
		i += n;
	}
	return out;
}

bool TextSystem::options_dirty() {
	const uint8_t *p = engine.vm.mem().host(engine.opt_addr(Engine::kOptBase), Engine::kOptSize);
	return impl->opt_snapshot.size() != size_t(Engine::kOptSize) ||
	       memcmp(impl->opt_snapshot.data(), p, Engine::kOptSize) != 0;
}

void TextSystem::clear_options_dirty() {
	const uint8_t *p = engine.vm.mem().host(engine.opt_addr(Engine::kOptBase), Engine::kOptSize);
	impl->opt_snapshot.assign(p, p + Engine::kOptSize);
}

}  // namespace ore
