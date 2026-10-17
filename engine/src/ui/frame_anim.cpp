#include "ui/frame_anim.h"

#include "text/font.h"

#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/layer.h"
#include "gfx/surface.h"

namespace ore {

Layer *layer_of(uint32_t h);  // gfx/natives_gfx.cpp

int FrameAnim::init(const std::string &path, int nframes, int ms, uint32_t attach, ImageRef page, int nx, int ny,
                    int nmode, int nend, bool nkeyed, bool keep8, uint32_t ck) {
	if (path.empty() || ms == 0) return 1;
	sprites.clear();
	if (attach) {
		auto *s = dynamic_cast<Sprite *>(layer_of(attach));
		target = s ? s->image : nullptr;
		if (!target) return 2;
		sprites.push_back(attach);
	} else {
		if (!page) return 1;
		target = std::move(page);
	}
	image = engine.load_image(path, keep8);
	if (!image) return 3;
	keyed = nkeyed ? 1 : 0;
	if (nkeyed && ck != 0xffffffffu) {
		image->color_key = ck & 0xffffff;  // vt+0x20(r, g, b): 32 bpp key with alpha 0
		keyed = 2;
	}
	frames = nframes > 0 ? nframes : 1;
	interval = ms;
	mode = nmode;
	frame_w = image->width() / frames;
	if (nx == 0xffff) {
		x = image->offset_x;
		y = image->offset_y;
	} else {
		x = nx;
		y = ny;
	}
	end_mode = nend;
	saved.reset();
	if (end_mode == 2 || keyed) saved = std::make_shared<Image>(frame_w, image->height(), target->bpp());
	return 0;
}

void FrameAnim::add_sprite(uint32_t h) {
	if (!h) return;
	for (uint32_t s : sprites)
		if (s == h) return;
	sprites.push_back(h);
}

void FrameAnim::remove_sprite(uint32_t h) {
	for (auto it = sprites.begin(); it != sprites.end(); ++it)
		if (*it == h) {
			sprites.erase(it);
			return;
		}
}

void FrameAnim::touch_sprites() {
	for (uint32_t h : sprites)
		if (Layer *l = layer_of(h)) l->touch();
	gfx_touch();
}

void FrameAnim::save_background() {
	if (!saved || !target) return;
	copy_rect(*saved, 0, 0, *target, Rect(x, y, x + frame_w, y + saved->height()));
}

void FrameAnim::draw_frame(int f) {
	if (!image || !target) return;
	Rect src(frame_w * f, 0, frame_w * (f + 1), image->height());
	if (keyed && saved) copy_rect(*target, x, y, *saved, saved->rect());
	if (keyed == 1) {
		// ARGB "over": the target may be a window surface with its own alpha
		Rect d = Rect(x, y, x + src.w(), y + src.h()).intersect(target->rect());
		for (int py = d.t; py < d.b; ++py)
			for (int px = d.l; px < d.r; ++px) {
				uint32_t s = image->pixel(src.l + px - x, src.t + py - y);
				if (s >> 24) blend_text_pixel(target->row32(py)[px], s & 0xffffff, int(s >> 24), true);
			}
	} else {
		blit(*target, *image, x, y, src, kBlendCopy, 255, false);
	}
	touch_sprites();
}

void FrameAnim::finish() {
	if (!image || !target || end_mode == 1) return;
	if (end_mode < 2) {
		draw_frame(0);
		return;
	}
	if (saved) copy_rect(*target, x, y, *saved, saved->rect());
	touch_sprites();
}

int FrameAnim::cycle_ms() const {
	if (mode == 0) return interval * frames;
	if (mode == 1) return (frames * 2 - 1) * interval;
	return 0;
}

int FrameAnim::frame_at(int64_t t) const {
	if (interval <= 0) return 0;
	int c = cycle_ms();
	if (c > 0) t %= c;
	int f = int(t / interval);
	if (f >= frames) f = frames * 2 - f - 2;
	if (f < 0) f = 0;
	return f;
}

// ---------------------------------------------------------------- FrameAnimTask

FrameAnimTask::FrameAnimTask(FrameAnim *anim, int loops, int type)
    : Task(type), anim_(anim ? anim->handle() : 0), loops_(loops) {
	if (anim) anim->save_background();
}

bool FrameAnimTask::update(int64_t now) {
	FrameAnim *a = Object::as<FrameAnim>(anim_);
	if (!a) return true;
	if (start_ < 0) start_ = now;
	int64_t t = now - start_;
	int c = a->cycle_ms();
	if (c > 0 && loops_ > 0 && t >= int64_t(c) * loops_) {
		a->finish();
		return true;
	}
	int f;
	if (c > 0) {
		f = a->frame_at(t);
	} else {  // random frame each interval (FUN_00457bc0)
		if (last_ >= 0 && t < int64_t(a->interval)) return false;
		start_ = now;
		rng_ = rng_ * 1103515245u + 12345u;
		f = int((rng_ >> 16) % uint32_t(a->frames));
		if (f == last_) f = (f + 1) % a->frames;
	}
	if (f != last_) {
		a->draw_frame(f);
		last_ = f;
	}
	return false;
}

void FrameAnimTask::finish() {
	if (FrameAnim *a = Object::as<FrameAnim>(anim_)) a->finish();
}

}  // namespace ore
