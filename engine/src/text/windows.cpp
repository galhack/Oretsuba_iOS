// ADV windows (docs/re/40_text.md §5-6).
#include "text/windows.h"

#include <algorithm>
#include <cstring>

#include "core/engine.h"
#include "gfx/blend.h"
#include "gfx/surface.h"
#include "text/font.h"
#include "text/message.h"
#include "text/text_system.h"
#include "ui/frame_anim.h"
#include "util/log.h"

namespace ore {

Layer *layer_of(uint32_t h);  // gfx/natives_gfx.cpp

namespace {

// Kinsoku tables of the EXE (0x4fc568..0x4fc57c), SJIS.
const char kOpenW[] = "\x81\x65\x81\x67\x81\x69\x81\x6b\x81\x6d\x81\x6f\x81\x71\x81\x73\x81\x75\x81\x77\x81\x79\x81\x8f"
                      "\x81\x90\x81\x92";
const char kOpenN[] = "\\$([{\xa2";
const char kCloseW[] = "\x81\x42\x81\x43\x81\x41\x81\x44\x81\x46\x81\x47\x81\x4a\x81\x4b\x81\x52\x81\x53\x81\x54\x81\x55"
                       "\x81\x58\x81\x66\x81\x68\x81\x6a\x81\x6c\x81\x6e\x81\x70\x81\x72\x81\x74\x81\x76\x81\x78\x81\x7a"
                       "\x81\x8b\x81\x8c\x81\x8d\x81\x8e\x81\x91\x81\x93\x81\xf1";
const char kCloseN[] = "%),:;]}\xa1\xa3\xde\xdf";
const char kSmallW[] = "\x81\x45\x81\x48\x81\x49\x81\x5b\x82\x9f\x82\xa1\x82\xa3\x82\xa5\x82\xa7\x82\xc1\x82\xe1\x82\xe3"
                       "\x82\xe5\x82\xec\x83\x40\x83\x42\x83\x44\x83\x46\x83\x48\x83\x62\x83\x83\x83\x85\x83\x87\x83\x8e"
                       "\x83\x95\x83\x96";
const char kSmallN[] = "!.?\xa4\xa5\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf\xb0";

// GBK equivalents for the Chinese patch (the patched EXE keeps the SJIS tables,
// which never match GBK punctuation; these keep 。，、 etc. off line starts).
const char kOpenGbk[] = "\xa3\xa8\xa1\xb8\xa1\xba\xa1\xbe\xa1\xb6\xa1\xb4\xa1\xb0\xa1\xae";
const char kCloseGbk[] = "\xa1\xa3\xa3\xac\xa1\xa2\xa3\xae\xa3\xba\xa3\xbb\xa3\xbf\xa3\xa1\xa3\xa9\xa1\xb9"
                         "\xa1\xbb\xa1\xbf\xa1\xb7\xa1\xb5\xa1\xb1\xa1\xaf\xa1\xad";

bool in_table(uint16_t c, const char *wide, const char *narrow) {
	if (c > 0xff) {
		for (const char *p = wide; p[0] && p[1]; p += 2)
			if ((uint16_t(uint8_t(p[0])) << 8 | uint8_t(p[1])) == c) return true;
		return false;
	}
	return c && strchr(narrow, char(c)) != nullptr;
}

int mask_at(const Image &m, int x, int y) {
	if (x < 0 || y < 0 || x >= m.width() || y >= m.height()) return 0;
	return int(m.pixel(x, y) & 0xff);
}

void blend_over(Image &dst, const Image &src) {
	int w = std::min(dst.width(), src.width()), h = std::min(dst.height(), src.height());
	for (int y = 0; y < h; ++y) {
		uint32_t *d = dst.row32(y);
		for (int x = 0; x < w; ++x) {
			uint32_t s = src.pixel(x, y);
			int a = src.bpp() == 32 && src.has_alpha ? int(s >> 24) : 255;
			blend_text_pixel(d[x], s & 0xffffff, a, true);
		}
	}
}

int key_held(Engine &e, bool ctrl) {
	return ctrl ? (e.input.key_held(0x1D) || e.input.key_held(0x9D)) : (e.input.key_held(0x2A) || e.input.key_held(0x36));
}

bool skip_key_held(Engine &e) {
	return key_held(e, e.opt8(0xeb8) != 0);
}

}  // namespace

// ---------------------------------------------------------------- FrameSet

bool FrameSet::load(Engine &e, const char *pb, const char *pa, const char *pg, int frames) {
	(void)frames;
	b = a = g = out = nullptr;
	if (pa && *pa) a = e.load_image(pa, true);
	if (pg && *pg) g = e.load_image(pg, true);
	if (pb && *pb) b = e.load_image(pb, false);
	const ImageRef &sz = a ? a : g ? g : b;
	if (!sz) return false;
	out = std::make_shared<Image>(sz->width(), sz->height(), 32);
	out->has_alpha = true;
	if (!a && !g && b) {
		for (int y = 0; y < out->height(); ++y)
			for (int x = 0; x < out->width(); ++x) out->row32(y)[x] = b->pixel(x, y) | (b->has_alpha ? 0 : 0xff000000u);
	}
	return true;
}

void FrameSet::compose(int mode, uint32_t rgb, int alpha) {
	if (!out) return;
	const ImageRef &m = mode ? a : g;
	if (!m) return;
	alpha = std::min(std::max(alpha, 0), 255);
	for (int y = 0; y < out->height(); ++y) {
		uint32_t *d = out->row32(y);
		for (int x = 0; x < out->width(); ++x) {
			int v = mask_at(*m, x, y);
			if (alpha < 255) v = v * alpha / 255;
			d[x] = uint32_t(v) << 24 | (rgb & 0xffffff);
		}
	}
	if (mode && b) blend_over(*out, *b);
}

// ---------------------------------------------------------------- TextWindow

TextWindow::TextWindow(Engine &e, UiSystem &ui) : UiControl(ui), engine(e) {}

TextWindow::~TextWindow() = default;

Font *TextWindow::font() const {
	return Object::as<Font>(font_h);
}

Font *TextWindow::ruby_font() const {
	return ruby_h ? Object::as<Font>(ruby_h) : nullptr;
}

int TextWindow::create(int x, int y, int w, int h, uint32_t fh, uint32_t rh, const char *b, const char *a,
                       const char *g, int nz) {
	if ((b && *b) || (a && *a) || (g && *g)) frame.load(engine, b, a, g);
	if (!Object::as<Font>(fh)) return 1;
	if (frame.out) {
		w = frame.out->width();
		h = frame.out->height();
	}
	if (w < 1 || h < 1) return 1;
	font_h = fh;
	ruby_h = rh;
	box_w = w;
	box_h = h;
	z = nz;
	surface = std::make_shared<Image>(w, h, 32);
	surface->has_alpha = true;
	sprite_ = std::make_unique<Sprite>(surface, nullptr, x, y);
	sprite_->mode = 2;
	sprite_->visible = false;  // shown by N_2d1 / the message window
	sprite_->tag = ui_.creation_tag;
	if (LayerList *list = ui_.parent_list()) list->insert(sprite_.get(), z, sprite_->tag);
	frame.compose(bg_mode, bg_rgb, bg_alpha);
	relayout();
	update_wrap();
	clear_page();
	return 0;
}

void TextWindow::touch() {
	gfx_touch();
}

void TextWindow::set_pos(int nx, int ny) {
	if (sprite_) sprite_->set_pos(nx, ny);
}

void TextWindow::set_visible(bool v) {
	if (!v) ui_.on_hidden(this);
	if (sprite_) sprite_->set_visible(v);
}

void TextWindow::set_z(int nz) {
	z = nz;
	if (sprite_ && sprite_->list()) sprite_->list()->reprioritize(sprite_.get(), z, true);
}

void TextWindow::set_bg_style(int mode, uint32_t rgb, int alpha) {
	rgb &= 0xffffff;
	alpha &= 0xff;
	if (mode == bg_mode && rgb == bg_rgb && alpha == bg_alpha) return;
	bg_mode = mode;
	bg_rgb = rgb;
	bg_alpha = alpha;
	frame.compose(bg_mode, bg_rgb, bg_alpha);
	touch();
}

void TextWindow::set_margins(int l, int t, int r, int b, bool redraw) {
	ml = l, mt = t, mr = r, mb = b;
	update_wrap();
	if (redraw) clear_page();
}

void TextWindow::set_pads(int l, int t, int r, int b, bool redraw) {
	pl = l, pt = t, pr = r, pb = b;
	relayout();
	if (redraw) clear_page();
}

void TextWindow::relayout() {
	Font *f = font();
	int h = f ? f->size : 16;
	if (Font *r = ruby_font()) h += ruby_gap + r->size;
	line_h = std::max(h, min_line_h);
	line_adv = line_h + pt + pb;
}

void TextWindow::update_wrap() {
	Font *f = font();
	wrap_x = box_w - (f ? f->size : 16) * wrap_reserve - mr;
}

void TextWindow::redraw_background() {
	if (!surface) return;
	if (frame.out)
		copy_rect(*surface, 0, 0, *frame.out, frame.out->rect());
	else
		surface->fill(bg_alpha ? (uint32_t(bg_alpha) << 24 | bg_rgb) : 0);
	prev_char = 0;
	next_overflow = false;
	relayout();
	touch();
}

void TextWindow::clear_page() {
	cx = ml;
	cy = mt;
	indent = 0;
	redraw_background();
}

bool TextWindow::newline() {
	int old_adv = line_adv, old_h = line_h;
	relayout();
	if (cy + line_h + line_spacing + old_adv > box_h - mb) {
		line_adv = old_adv;
		line_h = old_h;
		return true;
	}
	cy += line_spacing + line_adv;
	cx = indent + ml;
	prev_char = 0;
	if (cy + line_h + line_spacing + line_adv > box_h - mb) next_overflow = true;
	return false;
}

int TextWindow::put_char(uint16_t code) {
	Font *f = font();
	if (!f || !surface) return 0;
	int size = f->size, result = 0;
	if (autowrap && cx > wrap_x) {
		bool soft;
		if (engine.encoding == TextEncoding::Gbk)
			soft = in_table(prev_char, kOpenGbk, kOpenN) || in_table(code, kCloseGbk, kCloseN);
		else
			soft = in_table(prev_char, kOpenW, kOpenN) || in_table(code, kCloseW, kCloseN) ||
			       in_table(code, kSmallW, kSmallN);
		if (!soft || cx + size > box_w - mr) {
			if (newline()) return 1;
			result = -1;
		}
	}
	int gy = cy + (line_h - size), adv;
	if (code > 0xff && fullwidth) {
		int w = f->advance(code);
		f->draw_char(*surface, cx + (size - w) / 2, gy, code, true);
		adv = size;
	} else {
		adv = f->draw_char(*surface, cx, gy, code, true);
	}
	prev_char = code;
	cx += char_spacing + adv;
	touch();
	return result;
}

void TextWindow::draw_string(const std::string &s, int x, int y) {
	Font *f = font();
	if (!f || !surface) return;
	if (x >= 0) cx = ml + x;
	if (y >= 0) cy = mt + y;
	relayout();
	prev_char = 0;
	int end = f->draw(*surface, cx, cy + (line_h - f->size), s, true);
	cx = end + char_spacing;
	touch();
}

void TextWindow::print_aligned(const std::string &s, int mode) {
	Font *f = font();
	if (!f) return;
	int tw = 0, th = 0, ox = 0, oy = 0;
	f->measure(s, tw, th, &ox, &oy);
	int aw = box_w - ml - mr + pl + pr, ah = box_h - mt - mb + pt + pb;
	int x = -1, y = -1;
	switch (mode) {
	case 0: x = (aw - tw) / 2 - ox, y = (ah - th) / 2 - oy; break;
	case 1: x = (aw - tw) / 2 - ox; break;
	case 2: x = 0; break;
	case 3: x = aw - tw; break;
	default: break;
	}
	draw_string(s, std::max(x, 0), y);
}

void TextWindow::set_value_str(const char *s) {
	if (!s) return;
	const char *p = s;
	while (*p) {
		uint16_t c = Font::next_char(p, engine.encoding);
		if (c == '\r') continue;
		if (c == '\n')
			newline();
		else
			put_char(c);
	}
}

// ---------------------------------------------------------------- MessageWindow

MessageWindow::MessageWindow(Engine &e, UiSystem &ui, MessageManager &m) : TextWindow(e, ui), mm(m) {}

MessageWindow::~MessageWindow() {
	if (ctrl) {
		ctrl->win = nullptr;
		engine.tasks.kill(ctrl);
	}
	if (Task *t = Object::as<Task>(cursor_task)) engine.tasks.kill(t);
}

void MessageWindow::set_pos(int nx, int ny) {
	if (TextWindow *nb = Object::as<TextWindow>(name_box)) nb->set_pos(nb->x() + nx - x(), nb->y() + ny - y());
	TextWindow::set_pos(nx, ny);
}

void MessageWindow::clear_page() {
	TextWindow::clear_page();
	if (TextWindow *nb = Object::as<TextWindow>(name_box)) {
		nb->set_visible(false);
		has_name = false;
	}
}

void MessageWindow::set_cursor(uint32_t anim) {
	if (Task *t = Object::as<Task>(cursor_task)) engine.tasks.kill(t);
	cursor_task = 0;
	cursor = anim;
	cursor_visible = false;
	if (Layer *l = layer_of(anim)) l->set_visible(false);
}

// The cursor animation draws into this window's surface; it is shown by
// resuming its task and hidden by restoring the background and pausing it
// (FUN_00409b00, FUN_00409090, FUN_00409370).
void MessageWindow::set_cursor_visible(bool v) {
	cursor_visible = v;
	FrameAnim *a = Object::as<FrameAnim>(cursor);
	if (!a) {
		if (Layer *l = layer_of(cursor)) l->set_visible(v);
		return;
	}
	Task *t = Object::as<Task>(cursor_task);
	if (!t || t->killed || !engine.tasks.contains(t)) {
		auto nt = std::make_unique<FrameAnimTask>(a, 0);
		t = nt.get();
		cursor_task = t->handle();
		engine.tasks.add(std::move(nt));
		engine.tasks.pause(t);
	}
	if (v) {
		if (t->paused) engine.tasks.resume(t);
	} else {
		a->finish();
		if (!t->paused) engine.tasks.pause(t);
	}
}

bool MessageWindow::advance_pressed() {
	if (!visible() || !enabled()) return false;
	Input &in = engine.input;
	int dk = engine.opt8(0xeaa);
	if (in.key_pressed(0x1C) || in.key_pressed(0x9C) || (dk && in.key_pressed(dk))) return true;
	if (in.key_pressed(0xD1) || (engine.opt8(0xecd) && in.mouse_pressed(4))) return true;
	if (!in.mouse_pressed(0)) return false;
	int mx = in.cursor_x(), my = in.cursor_y();
	if (engine.ui)
		for (const auto &c : engine.ui->controls())
			if (c->in_group && c->ui_type() >= 0 && c->ui_type() < 2 && c->visible() && c->hit_test(mx, my))
				return false;
	return true;
}

int MessageWindow::show(bool allow_skip) {
	if (!ctrl) {
		auto t = std::make_unique<RevealTask>(this);
		ctrl = t.get();
		engine.tasks.add(std::move(t));
	}
	return start(false, allow_skip);
}

int MessageWindow::show_instant() {
	if (!ctrl) {
		auto t = std::make_unique<RevealTask>(this);
		ctrl = t.get();
		engine.tasks.add(std::move(t));
	}
	return start(true, true);
}

int MessageWindow::start(bool instant, bool allow_skip) {
	RevealTask *c = ctrl;
	c->restore_size();
	clear_page();
	if (cursor_visible) set_cursor_visible(false);
	if (!visible()) set_visible(true);
	if (TextWindow *nb = Object::as<TextWindow>(name_box)) {
		nb->clear_page();
		std::string nm = mm.name_segment(1);
		has_name = !nm.empty();
		if (has_name) {
			if (name_mode < 4)
				nb->print_aligned(nm, name_mode);
			else
				nb->draw_string(nm, 0, 0);
			if (!nb->visible()) nb->set_visible(true);
		} else if (nb->visible()) {
			nb->set_visible(false);
		}
	}
	std::string v = mm.name_segment(2);
	if (v.size() >= 16) return 2;
	voice_id = v;
	c->read = mm.read;
	if (instant) {
		c->reveal_page();
		return 0;
	}
	UiControl *btn = Object::as<UiControl>(skip_button);
	bool skip_key = skip_key_held(engine) && (!engine.opt8(0xeb9) || c->read);
	c->auto_skip = engine.opt8(0xe59) && c->read && allow_skip;
	if (!c->read) {
		if (btn) {
			if (!engine.opt8(0xe45)) {
				btn->set_enabled(true);
			} else {
				btn->set_enabled(false);
				btn->set_value_int(0);
			}
		}
		c->no_typewriter = engine.opt8(0xe55) != 0;
		c->ms = engine.opt8(0xe56);
	} else {
		c->no_typewriter = engine.opt8(0xe57) != 0;
		c->ms = engine.opt8(0xe58);
		if (c->auto_skip && advance_pressed()) c->auto_skip = false;
		if (btn) btn->set_enabled(true);
	}
	bool btn_skip = btn && btn->enabled() && btn->value() == 1;
	if (!c->auto_skip && !skip_key && !btn_skip) {
		c->st = 0;
		c->next_at = 0;
		c->stopped = false;
		return 0;
	}
	c->reveal_page();
	c->st = 4;
	return 0;
}

int MessageWindow::control(int mode) {
	if (!ctrl) return 1;
	switch (mode) {
	case 0: ctrl->reveal(true); return 0;
	case 1:
		for (int guard = 0; guard < 10000; ++guard) {
			ctrl->reveal(true);
			if (ctrl->last >= 2) break;
		}
		return 0;
	case 2: ctrl->reveal_page(); return 0;
	case 3: ctrl->leave_wait(); return 0;
	default: return 2;
	}
}

int MessageWindow::state() const {
	return ctrl ? ctrl->st : 0;
}

int MessageWindow::skip_flag() const {
	return ctrl ? ctrl->auto_skip : 0;
}

// ---------------------------------------------------------------- RevealTask

void RevealTask::restore_size() {
	if (!win || !saved_size) return;
	if (Font *f = win->font()) f->set_size(saved_size);
	win->relayout();
	saved_size = 0;
}

int RevealTask::reveal(bool all) {
	if (!win) return 1;
	MessageManager &mm = win->mm;
	for (int guard = 0; guard < 100000; ++guard) {
		MsgToken t;
		int code = mm.step(t);
		switch (code) {
		case kMsgChar: {
			int r;
			if (t.ch == '\\') {
				r = win->newline() ? 1 : 0;
			} else {
				r = win->put_char(t.ch);
				if (!all && r < 1) {
					last = 0;
					return 0;
				}
			}
			if (r < 1) continue;
			mm.skip_to_end();
			code = kMsgWaitEnd;
		}
			// fallthrough
		case kMsgWait:
		case kMsgWaitPage:
		case kMsgWaitEnd:
			last = code;
			st = 2;
			if (!win->cursor_visible) win->set_cursor_visible(true);
			return 0;
		case kMsgClear: win->clear_page(); continue;
		case kMsgEnd:
			stopped = true;
			last = kMsgEnd;
			st = 3;
			restore_size();
			return 0;
		case kMsgString:
			win->draw_string(t.str, -1, -1);
			if (!all) {
				last = 0;
				return 0;
			}
			continue;
		case kMsgSize:
			if (Font *f = win->font()) {
				if (!saved_size) saved_size = f->size;
				f->set_size(t.value + f->size);
				win->relayout();
			}
			continue;
		default: continue;
		}
	}
	last = 0xff;
	return 1;
}

int RevealTask::reveal_page() {
	if (!win) return 1;
	MessageManager &mm = win->mm;
	for (int guard = 0; guard < 100000; ++guard) {
		MsgToken t;
		int code = mm.step(t);
		if (code == kMsgChar) {
			int r = t.ch == '\\' ? (win->newline() ? 1 : 0) : win->put_char(t.ch);
			if (r > 0) {
				mm.skip_to_end();
				break;
			}
			continue;
		}
		if (code == kMsgWait || code == kMsgWaitPage || code == kMsgWaitEnd || code == kMsgBlock) continue;
		if (code == kMsgSize) {
			if (Font *f = win->font()) {
				if (!saved_size) saved_size = f->size;
				f->set_size(t.value + f->size);
				win->relayout();
			}
			continue;
		}
		if (code == kMsgString) win->draw_string(t.str, -1, -1);
		break;  // clear / end / string end the instant page
	}
	stopped = true;
	restore_size();
	st = 3;
	return 0;
}

void RevealTask::leave_wait() {
	if (st != 2 || !win) return;
	if (win->cursor_visible) win->set_cursor_visible(false);
	st = 1;
}

bool RevealTask::update(int64_t now) {
	if (!win) return true;
	if (stopped || !win->enabled()) return false;
	Engine &e = win->engine;
	bool skip_key = skip_key_held(e);
	UiControl *btn = Object::as<UiControl>(win->skip_button);
	bool skip_btn = btn && btn->enabled() && btn->value() == 1;
	if ((skip_key && (!e.opt8(0xeb9) || read)) || skip_btn) {
		reveal_page();
		st = 4;
		return false;
	}
	if (st == 1) {
		if (win->click_completes && win->advance_pressed()) {
			reveal(true);
			return false;
		}
	} else if (st == 2) {
		if (!win->advance_pressed()) return false;
		if (win->cursor_visible) win->set_cursor_visible(false);
		st = 1;
	}
	if (next_at <= now) {
		st = 1;
		reveal(no_typewriter);
		next_at = now + ms;
	}
	return false;
}

// ---------------------------------------------------------------- SelectWindow

SelectWindow::SelectWindow(Engine &e, UiSystem &ui) : UiControl(ui), engine(e) {}

SelectWindow::~SelectWindow() {
	if (engine.text && engine.text->active_select == handle()) engine.text->active_select = 0;
}

int SelectWindow::create(int x, int y, int w, int h, int sp, uint32_t font, int z) {
	if (!Object::as<Font>(font)) return 1;
	x_ = x, y_ = y, w_ = w, h_ = h;
	spacing = sp;
	font_h = font;
	z_ = z;
	// remember the parent page by handle: it may be destroyed before us
	page_handle_ = ui_.parent_surface() ? ui_.parent_surface()->handle() : 0;
	page_list_b_ = ui_.parent_b;
	return 0;
}

void SelectWindow::attach(Sprite *s) {
	s->mode = 2;
	s->tag = ui_.creation_tag;
	Surface *page = Object::as<Surface>(page_handle_);
	if (!page || !page->comp()) return;
	LayerList *list = page_list_b_ && page->overlay_list() ? page->overlay_list() : &page->comp()->main;
	list->insert(s, z_, s->tag);
}

int SelectWindow::set_images(int nframes, bool pressed, bool disabled_frame, const char *b1, const char *a1,
                             const char *g1, const char *b0, const char *a0, const char *g0, const char *b2,
                             const char *a2, const char *g2) {
	if (nframes < 1 || nframes > 4) return 1;
	frames = nframes;
	has_pressed = pressed;
	has_disabled = disabled_frame;
	item_bg_ = FrameSet();
	top_ = FrameSet();
	bottom_ = FrameSet();
	top_spr_.reset();
	bottom_spr_.reset();
	item_bg_.load(engine, b1, a1, g1);
	if (top_.load(engine, b0, a0, g0)) {
		Rect r(0, 0, top_.out->width() / frames, top_.out->height());
		top_spr_ = std::make_unique<Sprite>(top_.out, &r, x_, y_);
		attach(top_spr_.get());
	}
	if (bottom_.load(engine, b2, a2, g2)) {
		Rect r(0, 0, bottom_.out->width() / frames, bottom_.out->height());
		bottom_spr_ = std::make_unique<Sprite>(bottom_.out, &r, x_, y_);
		attach(bottom_spr_.get());
	}
	refresh_frames();
	layout();
	return 0;
}

void SelectWindow::refresh_frames() {
	item_bg_.compose(bg_mode, bg_rgb, bg_alpha);
	top_.compose(bg_mode, bg_rgb, bg_alpha);
	bottom_.compose(bg_mode, bg_rgb, bg_alpha);
	gfx_touch();
}

void SelectWindow::set_bg_style(int mode, uint32_t rgb, int alpha) {
	bg_mode = mode;
	bg_rgb = rgb & 0xffffff;
	bg_alpha = alpha & 0xff;
	refresh_frames();
}

int SelectWindow::add_item(const char *text, bool disabled) {
	Font *f = Object::as<Font>(font_h);
	if (!f || !text) return 1;
	auto it = std::make_unique<Item>();
	it->disabled = disabled;
	const ImageRef &base = item_bg_.out;
	int n;
	if (!base) {
		it->fw = w_ > 0 ? w_ : 400;
		it->fh = h_ > 0 ? h_ : f->size + 8;
		n = 2;
		it->img = std::make_shared<Image>(it->fw * 2, it->fh, 32);
		it->img->fill(uint32_t(bg_alpha) << 24 | bg_rgb);
	} else if (frames < 2) {
		it->fw = base->width();
		it->fh = base->height();
		n = 2;
		it->img = std::make_shared<Image>(it->fw * 2, it->fh, 32);
		copy_rect(*it->img, 0, 0, *base, base->rect());
		copy_rect(*it->img, it->fw, 0, *base, base->rect());
	} else {
		n = frames;
		it->fw = base->width() / frames;
		it->fh = base->height();
		it->img = base->clone();
	}
	it->img->has_alpha = true;
	it->nframes = n;
	if (hl_mode) {
		uint32_t c = hl_mode == 1 ? (uint32_t(bg_alpha) << 24 | (hl_color & 0xffffff)) : hl_color;
		int last = (has_disabled && n > 2) ? n - 1 : n;
		for (int k = 1; k < last; ++k)
			fill_rect(*it->img, Rect(k * it->fw + ml, mt, (k + 1) * it->fw - mr, it->fh - mb), c);
	}
	bool chosen = false;
	if (mark_chosen && engine.text) chosen = engine.text->msg().choice_chosen(int(items_.size()));
	uint32_t save_color = f->color, save_top = f->grad_top, save_bottom = f->grad_bottom;
	auto use = [&](int which) {  // 0 normal, 1 chosen, 2 hover, 3 disabled
		const uint32_t cols[4] = {col_normal, col_chosen, col_hover, col_disabled};
		f->color = cols[which];
		f->grad_top = grad[which * 2];
		f->grad_bottom = grad[which * 2 + 1];
	};
	int x0 = tx + ml, y0 = ty + mt;
	use(disabled ? 3 : chosen ? 1 : 0);
	f->draw(*it->img, x0, y0, text, true);
	if (!disabled) use(2);
	f->draw(*it->img, it->fw + x0, y0, text, true);
	int k = 2;
	if (has_pressed && k < n) {
		f->draw(*it->img, k * it->fw + x0 + press_dx, y0 + press_dy, text, true);
		++k;
	}
	if (has_disabled && !disabled && k < n) {
		use(chosen ? 1 : 0);
		f->draw(*it->img, k * it->fw + x0, y0, text, true);
	}
	f->color = save_color;
	f->grad_top = save_top;
	f->grad_bottom = save_bottom;
	Rect r(0, 0, it->fw, it->fh);
	it->spr = std::make_unique<Sprite>(it->img, &r, x_, y_);
	it->spr->visible = visible_;
	attach(it->spr.get());
	items_.push_back(std::move(it));
	layout();
	return 0;
}

void SelectWindow::clear() {
	items_.clear();
	hover = pressed_ = focus_ = -1;
	static_mode = false;
	layout();
	gfx_touch();
}

void SelectWindow::set_frame(Item &it, int f) {
	if (f >= it.nframes) f = it.nframes - 1;
	if (f < 0) f = 0;
	it.frame = f;
	it.spr->src = Rect(f * it.fw, 0, (f + 1) * it.fw, it.fh);
	it.spr->touch();
}

int SelectWindow::highlight(int i, bool on) {
	if (i < 0 || i >= int(items_.size())) return -1;
	set_frame(*items_[size_t(i)], on ? 1 : 0);
	return 0;
}

void SelectWindow::layout() {
	int y = y_;
	bool any = !items_.empty();
	if (top_spr_) {
		top_spr_->set_pos(x_, y);
		top_spr_->set_visible(visible_ && any);
		y += top_spr_->src.h();
	}
	for (auto &it : items_) {
		it->spr->set_pos(x_, y);
		it->spr->set_visible(visible_);
		y += it->fh + spacing;
	}
	if (bottom_spr_) {
		bottom_spr_->set_pos(x_, y);
		bottom_spr_->set_visible(visible_ && any);
	}
	gfx_touch();
}

int SelectWindow::width() const {
	if (w_ > 0) return w_;
	if (!items_.empty()) return items_[0]->fw;
	if (top_spr_) return top_spr_->src.w();
	return 0;
}

int SelectWindow::height() const {
	int h = 0;
	if (top_spr_) h += top_spr_->src.h();
	for (auto &it : items_) h += it->fh + spacing;
	if (bottom_spr_) h += bottom_spr_->src.h();
	return h;
}

bool SelectWindow::item_pos(int i, int &px, int &py) {
	if (i < 0 || i >= int(items_.size())) return false;
	const Item &it = *items_[size_t(i)];
	px = it.spr->x + ml + 8;
	py = it.spr->y + it.fh / 2;
	return true;
}

void SelectWindow::set_pos(int nx, int ny) {
	x_ = nx;
	y_ = ny;
	layout();
}

void SelectWindow::set_visible(bool v) {
	if (!v) ui_.on_hidden(this);
	visible_ = v;
	layout();
}

void SelectWindow::set_z(int nz) {
	z_ = nz;
	if (top_spr_) attach(top_spr_.get());
	if (bottom_spr_) attach(bottom_spr_.get());
	for (auto &it : items_) attach(it->spr.get());
}

int SelectWindow::item_at(int lx, int ly) const {
	int sx = lx + x_, sy = ly + y_;
	for (size_t i = 0; i < items_.size(); ++i) {
		const Item &it = *items_[i];
		if (it.spr->bounds().contains(sx, sy)) return int(i);
	}
	return -1;
}

bool SelectWindow::hit_test(int sx, int sy) const {
	return visible_ && item_at(sx - x_, sy - y_) >= 0;
}

void SelectWindow::on_mouse_move(int lx, int ly) {
	if (static_mode) return;
	int i = item_at(lx, ly);
	if (i >= 0 && items_[size_t(i)]->disabled) i = -1;
	if (i == hover) return;
	if (hover >= 0 && hover < int(items_.size())) set_frame(*items_[size_t(hover)], 0);
	hover = i;
	focus_ = i;
	if (hover >= 0) set_frame(*items_[size_t(hover)], 1);
}

void SelectWindow::on_mouse_leave() {
	if (static_mode) return;
	if (hover >= 0 && hover < int(items_.size())) set_frame(*items_[size_t(hover)], 0);
	hover = pressed_ = -1;
}

bool SelectWindow::on_mouse(int lx, int ly) {
	if (static_mode) return false;
	int i = item_at(lx, ly);
	if (i < 0 || items_[size_t(i)]->disabled) return false;
	if (!ui_.mouse_pressed(0)) return false;
	pressed_ = i;
	set_frame(*items_[size_t(i)], has_pressed ? 2 : 1);
	if (id >= 0) ui_.set_event(id + i);
	return true;
}

bool SelectWindow::on_key() {
	if (static_mode || items_.empty()) return false;
	int n = int(items_.size());
	auto move = [&](int d) {
		int i = focus_;
		for (int k = 0; k < n; ++k) {
			i = i < 0 ? (d > 0 ? 0 : n - 1) : (i + d + n) % n;
			if (!items_[size_t(i)]->disabled) break;
		}
		if (focus_ >= 0 && focus_ < n) set_frame(*items_[size_t(focus_)], 0);
		focus_ = hover = i;
		set_frame(*items_[size_t(i)], 1);
	};
	if (ui_.action_pressed(0)) {
		move(-1);
		return true;
	}
	if (ui_.action_pressed(1)) {
		move(1);
		return true;
	}
	if (ui_.action_pressed(4) && focus_ >= 0 && focus_ < n && !items_[size_t(focus_)]->disabled) {
		if (id >= 0) ui_.set_event(id + focus_);
		return true;
	}
	return false;
}

Layer *SelectWindow::sprite(int i) {
	if (i >= 0 && i < int(items_.size())) return items_[size_t(i)]->spr.get();
	return top_spr_.get();
}

ImageRef SelectWindow::image(int i) {
	if (i >= 0 && i < int(items_.size())) return items_[size_t(i)]->img;
	return item_bg_.out;
}

}  // namespace ore
