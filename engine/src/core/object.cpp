#include "core/object.h"

namespace ore {
namespace {

std::unordered_map<uint32_t, Object *> &registry() {
	static std::unordered_map<uint32_t, Object *> r;
	return r;
}

uint32_t g_next = 1;

}  // namespace

Object::Object() : handle_(kHandleBase + g_next++) {
	registry()[handle_] = this;
}

Object::~Object() {
	registry().erase(handle_);
}

Object *Object::from_handle(uint32_t h) {
	auto it = registry().find(h);
	return it == registry().end() ? nullptr : it->second;
}

}  // namespace ore
