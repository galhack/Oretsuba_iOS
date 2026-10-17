// Visual effects: transitions (N_fb), layer effects (N_d6..N_d9, N_35) and the
// image-processing natives (docs/re/30_effects.md).
#include "core/engine.h"
#include "gfx/effects/transitions.h"

namespace ore {

void register_effect_natives(Engine &e) {
	register_transition_natives(e);
	register_image_op_natives(e);
	register_layer_effect_natives(e);
}

}  // namespace ore
