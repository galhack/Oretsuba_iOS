#include "gfx/layer.h"

#include <algorithm>
#include <cmath>

#include "gfx/blend.h"
#include "gfx/surface.h"

namespace ore {

// ---------------------------------------------------------------- Layer

Layer::~Layer() {
	detach();
}

void Layer::detach() {
	if (list_) {
		list_->remove(this);
		gfx_touch();
	}
}

void Layer::touch() {
	gfx_touch();
}

// ---------------------------------------------------------------- LayerList

LayerList::~LayerList() {
	clear();
}

void LayerList::clear() {
	for (Node &n : nodes_) n.layer->list_ = nullptr;
	nodes_.clear();
}

void LayerList::insert(Layer *l, int prio, int tag, bool before_equals) {
	if (l->list_) l->list_->remove(l);
	auto it = nodes_.begin();
	if (before_equals)
		while (it != nodes_.end() && it->prio < prio) ++it;
	else
		while (it != nodes_.end() && it->prio <= prio) ++it;
	nodes_.insert(it, Node{l, prio, tag});
	l->list_ = this;
	gfx_touch();
}

void LayerList::swap_contents(LayerList &a, LayerList &b) {
	std::swap(a.nodes_, b.nodes_);
	for (Node &n : a.nodes_) n.layer->list_ = &a;
	for (Node &n : b.nodes_) n.layer->list_ = &b;
	gfx_touch();
}

bool LayerList::remove(Layer *l) {
	for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
		if (it->layer == l) {
			nodes_.erase(it);
			l->list_ = nullptr;
			gfx_touch();
			return true;
		}
	}
	return false;
}

bool LayerList::reprioritize(Layer *l, int prio, bool before_equals) {
	Node *n = find(l);
	if (!n) return false;
	int tag = n->tag;
	remove(l);
	insert(l, prio, tag, before_equals);
	return true;
}

LayerList::Node *LayerList::find(Layer *l) {
	for (Node &n : nodes_)
		if (n.layer == l) return &n;
	return nullptr;
}

// ---------------------------------------------------------------- Sprite

Sprite::Sprite(ImageRef img, const Rect *rect, int nx, int ny) {
	init(std::move(img), rect, nx, ny);
}

void Sprite::init(ImageRef img, const Rect *rect, int nx, int ny) {
	image = std::move(img);
	mode = 0;
	opacity = 255;
	zoom_w = zoom_h = 0;
	angle = 0;
	zoom = 1.0;
	scroll_x = scroll_y = 0;
	src = image ? image->rect() : Rect();
	if (rect && image) src = rect->intersect(image->rect());
	if (image && image->has_alpha) mode = 2;
	x = nx;
	y = ny;
	cx = src.w() / 2;
	cy = src.h() / 2;
	touch();
}

void Sprite::set_image_keep(ImageRef img) {
	image = std::move(img);
	touch();
}

Rect Sprite::bounds() const {
	int w = zoom_w ? zoom_w : src.w();
	int h = zoom_h ? zoom_h : src.h();
	if (angle == 0 && zoom == 1.0) return Rect(x, y, x + w, y + h);
	// rotated: bounding box of the transformed quad around the centre
	double a = angle * 2.0 * M_PI / 512.0;
	double c = std::cos(a) * zoom, s = std::sin(a) * zoom;
	double pts[4][2] = {{0.0 - cx, 0.0 - cy}, {double(w) - cx, 0.0 - cy}, {0.0 - cx, double(h) - cy}, {double(w) - cx, double(h) - cy}};
	double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
	for (auto &p : pts) {
		double X = c * p[0] + s * p[1] + cx, Y = -s * p[0] + c * p[1] + cy;
		minx = std::min(minx, X), maxx = std::max(maxx, X), miny = std::min(miny, Y), maxy = std::max(maxy, Y);
	}
	return Rect(x + int(std::floor(minx)), y + int(std::floor(miny)), x + int(std::ceil(maxx)), y + int(std::ceil(maxy)));
}

void Sprite::draw(Image &dst, const Rect &clip) {
	if (!image || !visible || opacity <= 0) return;
	if (angle == 0 && zoom == 1.0) {
		if (zoom_w || zoom_h) {
			int w = zoom_w ? zoom_w : src.w(), h = zoom_h ? zoom_h : src.h();
			stretch_blit(dst, *image, x, y, w, h, src, mode, filter & 0xf, opacity, false, &clip);
		} else {
			blit(dst, *image, x, y, src, mode, opacity, false, &clip);
		}
		return;
	}
	double a = angle * 2.0 * M_PI / 512.0;
	double c = std::cos(a) * zoom, s = std::sin(a) * zoom;
	double sx = zoom_w ? double(zoom_w) / src.w() : 1.0, sy = zoom_h ? double(zoom_h) / src.h() : 1.0;
	// X = c*(u*sx - cx) + s*(v*sy - cy) + cx + x ; Y = -s*(u*sx - cx) + c*(v*sy - cy) + cy + y
	double A = c * sx, B = s * sy, C = -s * sx, D = c * sy;
	double tx = -c * cx - s * cy + cx + x, ty = s * cx - c * cy + cy + y;
	affine_blit(dst, *image, src, A, B, C, D, tx, ty, mode, opacity, false, &clip);
}

bool Sprite::hit(int px, int py, bool use_pixels) const {
	Rect b = bounds();
	if (!b.contains(px, py)) return false;
	if (!use_pixels || !image) return true;
	int sx = src.l + (px - x), sy = src.t + (py - y);
	if (image->bpp() == 8) return image->color_key == 0xFFFFFFFF || image->row8(sy)[sx] != uint8_t(image->color_key);
	uint32_t p = image->pixel(sx, sy);
	if (image->color_key != 0xFFFFFFFF) return p != image->color_key;
	return (p >> 24) != 0;
}

std::unique_ptr<Sprite> Sprite::clone() const {
	auto s = std::make_unique<Sprite>(image, &src, x, y);
	s->tag = tag;
	s->visible = visible;
	s->mode = mode;
	s->opacity = opacity;
	s->zoom_w = zoom_w;
	s->zoom_h = zoom_h;
	s->angle = angle;
	s->zoom = zoom;
	s->cx = cx;
	s->cy = cy;
	s->filter = filter;
	s->scroll_x = scroll_x;
	s->scroll_y = scroll_y;
	return s;
}

// ---------------------------------------------------------------- ColorRect

ColorRect::ColorRect(int w_, int h_, uint32_t argb, int nx, int ny) : rgb(argb & 0xffffff), w(w_), h(h_) {
	opacity = int(argb >> 24);
	x = nx;
	y = ny;
}

void ColorRect::draw(Image &dst, const Rect &clip) {
	if (!visible || opacity <= 0) return;
	color_fill(dst, bounds().intersect(clip), rgb, opacity, mode);
}

}  // namespace ore
