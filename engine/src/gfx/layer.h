// Layer objects drawn by a compositor (docs/re/20_graphics_core.md §2.4-2.5).
#pragma once

#include <cstdint>
#include <vector>

#include "core/object.h"
#include "gfx/image.h"

namespace ore {

class LayerList;

class Layer : public Object {
public:
	~Layer() override;

	int tag = 0;
	bool visible = true;
	int x = 0, y = 0;
	int mode = 0;      // blend mode
	int opacity = 255;

	virtual Rect bounds() const = 0;
	virtual void draw(Image &dst, const Rect &clip) = 0;
	virtual bool hit(int px, int py, bool use_pixels) const { return bounds().contains(px, py) && (void(use_pixels), true); }
	virtual void set_pos(int nx, int ny) {
		x = nx;
		y = ny;
		touch();
	}
	virtual bool set_mode(int m) {
		mode = m;
		touch();
		return true;
	}
	void set_visible(bool v) {
		visible = v;
		touch();
	}
	void set_opacity(int o) {
		opacity = o < 0 ? 0 : o > 255 ? 255 : o;
		touch();
	}

	LayerList *list() const { return list_; }
	void detach();
	// Marks the containing page as needing composition.
	void touch();

private:
	friend class LayerList;
	LayerList *list_ = nullptr;
};

// One of a compositor's two sorted lists.
class LayerList {
public:
	explicit LayerList(class Compositor *owner) : owner_(owner) {}
	~LayerList();

	struct Node {
		Layer *layer;
		int prio;
		int tag;
	};
	// before_equals: inserted in front of nodes with the same priority.
	void insert(Layer *l, int prio, int tag, bool before_equals = true);
	bool remove(Layer *l);
	bool reprioritize(Layer *l, int prio, bool before_equals);
	Node *find(Layer *l);
	const std::vector<Node> &nodes() const { return nodes_; }
	std::vector<Node> &nodes() { return nodes_; }
	void clear();
	// Exchanges the contents of two lists (layers follow their nodes).
	static void swap_contents(LayerList &a, LayerList &b);
	Compositor *owner() const { return owner_; }
	void set_owner(Compositor *c) { owner_ = c; }

private:
	Compositor *owner_;
	std::vector<Node> nodes_;  // ascending priority
};

class Sprite : public Layer {
public:
	Sprite(ImageRef image, const Rect *src, int x, int y);
	ImageRef image;
	Rect src;             // source rect within image
	int zoom_w = 0, zoom_h = 0;  // target size, 0 = none
	int angle = 0;        // 512 = 360 degrees
	double zoom = 1.0;
	int cx = 0, cy = 0;   // rotation centre (source pixels)
	int filter = 0;
	int scroll_x = 0, scroll_y = 0;

	Rect bounds() const override;
	void draw(Image &dst, const Rect &clip) override;
	bool hit(int px, int py, bool use_pixels) const override;
	void init(ImageRef img, const Rect *rect, int nx, int ny);
	void set_image_keep(ImageRef img);
	std::unique_ptr<Sprite> clone() const;
};

class ColorRect : public Layer {
public:
	ColorRect(int w, int h, uint32_t argb, int x, int y);
	uint32_t rgb;
	int w, h;
	Rect bounds() const override { return Rect(x, y, x + w, y + h); }
	void draw(Image &dst, const Rect &clip) override;
	bool set_mode(int m) override {
		if (m > 3) return false;
		return Layer::set_mode(m);
	}
};

}  // namespace ore
