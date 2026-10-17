// LAG animations, LAD resources and the lip-sync task (docs/re/40_text.md §8).
#include "text/lag.h"

#include "core/engine.h"
#include "gfx/surface.h"
#include "text/text_system.h"
#include "ui/frame_anim.h"
#include "util/bytes.h"
#include "util/log.h"

namespace ore {

// ---------------------------------------------------------------- LagData

// "LAG\0", {u16 w, u16 h, u16 ?, u16 nSeq, u32 ?}, u32 skip + skip bytes,
// u32 n, n x {i32 image, u32 ms, s16 x, s16 y, u32 ?} (image < 0 ends a
// sequence), u32 nImages, nImages x {u32 size, u32 offset, s16 x, s16 y}
// with embedded ELG data at offset.
bool LagData::load(Engine &e, const std::string &p, bool alpha_images) {
	std::vector<uint8_t> d;
	if (!e.read_file(p, d)) {
		log_warn("lag: missing %s", p.c_str());
		return false;
	}
	path = p;
	ByteReader r(d);
	if (r.u32() != 0x47414c) return false;
	width = r.u16();
	height = r.u16();
	r.u16();
	int nseq = r.u16();
	r.u32();
	if (nseq == 0) return false;
	r.skip(r.u32());
	uint32_t n = r.u32();
	if (n > 100000) return false;
	entries.clear();
	seq_start.assign(1, 0);
	for (uint32_t i = 0; i < n; ++i) {
		Entry en;
		en.image = r.i32();
		en.ms = r.u32();
		en.x = int16_t(r.u16());
		en.y = int16_t(r.u16());
		r.u32();
		entries.push_back(en);
		if (en.image < 0 && int(seq_start.size()) < nseq) seq_start.push_back(int(i + 1));
	}
	entries.push_back(Entry());
	uint32_t ni = r.u32();
	if (r.bad() || ni > 100000) return false;
	images.clear();
	for (uint32_t i = 0; i < ni && !r.bad(); ++i) {
		uint32_t size = r.u32(), off = r.u32();
		int x = int16_t(r.u16()), y = int16_t(r.u16());
		ImageRef img;
		if (off < d.size() && size <= d.size() - off) {
			std::vector<uint8_t> elg(d.begin() + long(off), d.begin() + long(off + size));
			img = Image::from_elg(elg, false);
		}
		if (img) {
			if (x >= 0) {
				img->offset_x = x;
				img->offset_y = y;
			}
			if (alpha_images || x == -2) img->has_alpha = img->bpp() == 32;
			else img->has_alpha = false;
		}
		images.push_back(img);
	}
	return !r.bad();
}

// ---------------------------------------------------------------- LagPlayer

int LagPlayer::start(LagData *d, LayerList *list, int seq, int nloops, bool cu, bool rm) {
	if (!d || !list) return 1;
	if (seq < 0 || seq >= int(d->seq_start.size())) return 2;
	first = cur = size_t(d->seq_start[size_t(seq)]);
	if (cur >= d->entries.size() || d->entries[cur].image < 0) return 3;
	data = d->handle();
	loops = nloops;
	catch_up = cu;
	remove_at_end = rm;
	show(cur);
	list->insert(&sprite, 0, 0);
	return 0;
}

void LagPlayer::show(size_t idx) {
	LagData *d = Object::as<LagData>(data);
	if (!d || idx >= d->entries.size()) return;
	const LagData::Entry &en = d->entries[idx];
	ImageRef img = en.image >= 0 && size_t(en.image) < d->images.size() ? d->images[size_t(en.image)] : nullptr;
	bool vis = sprite.visible;
	sprite.init(img, nullptr, en.x, en.y);
	sprite.visible = vis;
	cur = idx;
}

void LagPlayer::finish() {
	sprite.detach();
	state = 2;
}

bool LagPlayer::update(int64_t now) {
	LagData *d = Object::as<LagData>(data);
	if (!d) {
		finish();
		return true;
	}
	if (state != 1) {
		if (state == 0) {
			state = 1;
			t0 = now;
			return false;
		}
		if (!remove_at_end) return false;
		finish();
		return true;
	}
	// the data may have been reloaded with fewer frames (N_1fb)
	if (cur >= d->entries.size()) {
		finish();
		return true;
	}
	auto ended = [d](size_t i) { return i >= d->entries.size() || d->entries[i].image < 0; };
	size_t nxt = cur;
	int64_t el = now - t0;
	auto wrap = [&](size_t &p) -> int {  // 0 continue, 1 stop at last frame, 2 finished
		if (loops < 0) {
			p = first;
			return 0;
		}
		if (loops < 1) {
			state = 2;
			p = p - 1;
			return 1;
		}
		if (--loops == 0) return 2;
		p = first;
		return 0;
	};
	if (!catch_up) {
		if (int64_t(d->entries[cur].ms) <= el) {
			nxt = cur + 1;
			t0 = now;
			if (ended(nxt)) {
				int w = wrap(nxt);
				if (w == 2) {
					finish();
					return true;
				}
			}
		}
	} else {
		for (int guard = 0; guard < 100000; ++guard) {
			if (nxt >= d->entries.size()) break;
			uint32_t ms = d->entries[nxt].ms;
			if (int64_t(ms) > el) break;
			el -= ms;
			t0 += ms;
			++nxt;
			if (ended(nxt)) {
				int w = wrap(nxt);
				if (w == 2) {
					finish();
					return true;
				}
				if (w == 1) break;
			}
		}
	}
	if (nxt != cur && nxt < d->entries.size()) show(nxt);
	return false;
}

// ---------------------------------------------------------------- LipSyncTask

bool LipSyncTask::update(int64_t) {
	FrameAnim *a = Object::as<FrameAnim>(target_);
	if (!a || !e_.sound.channel_playing(ch_)) {
		finish();
		return true;
	}
	int f = e_.sound.channel_lip_level(ch_, a->frames);
	if (f != frame_) {
		a->draw_frame(f);  // FUN_004580f0
		frame_ = f;
	}
	return false;
}

void LipSyncTask::finish() {
	if (FrameAnim *a = Object::as<FrameAnim>(target_))
		if (frame_ != 0) a->draw_frame(0);
	frame_ = 0;
}

}  // namespace ore
