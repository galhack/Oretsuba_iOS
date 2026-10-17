// Transition factory and the effect natives' registration helpers.
#pragma once

#include <memory>
#include <string>

#include "core/args.h"
#include "gfx/image.h"

namespace ore {

class Engine;
class Transition;

// Creates the named transition from args[k..] (args[k] = duration ms); null if unknown.
std::unique_ptr<Transition> make_transition(Engine &e, const std::string &name, Args &a, int k);
// Same with the rule image already resolved (used by the tests).
std::unique_ptr<Transition> make_transition(const std::string &name, Args &a, int k, ImageRef rule);

// tvpgl "rule" (universal transition): s1 → s2 through an 8 bpp rule image with
// progress a (0..255) and vague width (docs/re/30_effects.md §2.3).
void rule_blend(Image &dst, const Image &s1, const Image &s2, const Image &rule, int a, int vague, int dx, int dy,
                int w, int h, int s1x, int s1y, int s2x, int s2y, int rx, int ry);

void register_transition_natives(Engine &e);
void register_image_op_natives(Engine &e);
void register_layer_effect_natives(Engine &e);

}  // namespace ore
