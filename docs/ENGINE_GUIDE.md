# Engine implementation guide (for contributors)

The engine is a C++17 reimplementation of the Lucifen engine used by
俺たちに翼はない (ORE_TUBA.EXE 1.01). Reverse-engineering specs are in
`docs/re/*.md`; read `00_vm.md` first, then the doc of your area.

## Layout (engine/src)
* `vm/` SOB virtual machine (`Vm`, `VmMemory` 32-bit arena). Script-visible
  memory lives in the arena; natives get `uint32_t args` (VM address).
* `core/engine.h` `Engine`: owns everything (vfs, vm, mixer, sound, input,
  `tasks` scheduler, `text`, `ui`, image/view/layer registries, framebuffer,
  option block). One display frame = `Engine::frame(now)`: input poll → sound
  update → `tasks.tick(now)` → VM run(s) → `render_frame()` (views →
  800×600 BGRA framebuffer).
* `core/object.h` every script-visible object derives from `Object` and gets
  a 32-bit handle (`handle()`, `Object::as<T>(h)`). Natives return handles.
* `core/args.h` `Args`: `a.i(k)`, `a.u(k)`, `a.s(k)` (C string), `a.out(k,v)`
  writes through an int* argument, `a.out_str(k, s, limit)`.
* `core/scheduler.h` `Task` (typed, `update(now)` returns true when done,
  `finish()` jumps to the end state) and `Scheduler` (`Engine::tasks` is the
  user task list DAT_00503214).
* `gfx/` `Image` (BGRA32 straight alpha or 8 bpp + palette), `blend.h`
  (tvpgl-exact kernels: `blit`, `stretch_blit`, `affine_blit`, `fill_rect`,
  `color_fill`, `copy_rect`, `blend_images`), `Layer`/`Sprite`/`ColorRect`,
  `LayerList` (ascending priority; drawn from the highest value to the
  lowest), `Compositor` (bg image/offset/colour, `bg_cache` = transformed or
  effect-written background drawn at 0,0, main + overlay lists), `Surface`
  (pages type 7/9/10 with `linked` back page and a `Transition`), `View`.
  Call `gfx_touch()` after changing anything visible.
* `gfx/transition.h` transition interface used by `Surface`.
* `audio/` mixer + sound manager; `input/` DIK-based input; `media/` movies.
* `text/` fonts + message system; `ui/` controls. `platform/` host services
  (`Platform`: CoreText glyphs, SJIS/GBK → Unicode, time, dialogs).

## Natives
Register in your area's `register_*_natives(Engine &e)` with the pattern used
in `core/natives_system.cpp`:
```cpp
auto def = [&e](int idx, const char *name, std::function<int32_t(Args &)> fn) {
	e.vm.set_native(idx, [fn](Vm &v, uint32_t a) { Args args(v, a); return fn(args); }, name);
};
def(0x1b1, "set_opacity", [](Args &a) { ... });
```
Argument k = k-th pushed word (args[0] first push). Strings are in the game
encoding (`e.encoding`: Shift-JIS, or GBK for the Chinese patch, lead bytes
0x81–0xFE). Handles of other subsystems: `e.image_of(h)` (image or page
pixels), `e.surface_of(h)`, `layer_of(h)` (gfx/natives_gfx.cpp), `Object::as<T>`.

## Build and test
```
make -C engine BUILD=../build/host-<yourname> -j8
../build/host-<yourname>/orehost --data data/SET_UP --frames 900 --shot 300,600 --out /tmp/<dir>
```
`orehost --input FILE` feeds scripted clicks/keys (see engine/host/main.mm).
The Chinese patch: `--patch 中文三号补丁 --chs`. Game data must never be
committed. Keep to the existing code style (tabs, 120 columns, brief
comments, names like the surrounding code).


## Movie regression and device diagnostics

`make -C engine tests` also builds `build/host/movie_flow`. Run it with the
player-owned SET_UP and Chinese patch folders. It runs both original MoviePlay
scripts to natural completion, checks that frames were rendered and the script
returns, and uses disposable saves. Its 60 Hz host mixes 735 audio samples per
frame at 44100 Hz.

Debug app builds accept `ORE_TEST_MOVIE=0` (first opening) or `1` (second opening)
in the launch environment, together with `ORE_AUTOSTART=1`. This uses a new
UUID-named temporary save directory, skips patch splash screens and enters the
original MoviePlay script. Launch again without the environment variables to
resume ordinary play. The diagnostic C API and environment entry are excluded
from Release builds; no movie or other game resource is bundled.
