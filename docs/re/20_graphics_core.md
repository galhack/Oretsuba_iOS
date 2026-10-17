# Graphics core: images, surfaces, layers, composition (ORE_TUBA.EXE 1.01)

Scope: the image / surface / layer-object model, the per-frame composition into the
800x600 frame, the blend math (incl. which `tvpgl.dll` entry points are used), the task
scheduler that drives animations, and every *used* native in `N_c5..N_10f` and
`N_18d..N_1c7`. Transition/effect internals (`cross`, `mosaic`, `bg_*` …, natives
`N_13b..N_18c`), text, and UI objects (`N_2a7..N_30c`) are covered by other docs; they
are referenced here only where they plug into this core.

Conventions: `obj+0xNN` = byte offset in the C++ object; `vt+0xNN` = virtual slot
(byte offset into the vtable). Addresses are VAs in ORE_TUBA.EXE unless prefixed `tvpgl:`
(tvpgl.dll, image base 0x10000000). Natives get `int *args`; `args[0]` = first push.
"verified" = read from decompilation/disassembly; "(guess)" = inferred.

Tooling note: many virtual methods were not functions in the shipped Ghidra project
(only reachable through vtables). They were decompiled with a scratch copy of the project
(script `DecompAddrs.java`: create function at address + decompile). Ghidra also
frequently *drops one stack argument* of `__thiscall` functions (e.g. 0x446690 really
takes 4 args, `ret 0x10`); argument lists below were checked against the `ret n` /
push sequences.

---------------------------------------------------------------------------------------

## 1. The singletons

`app` = DAT_00503248 (ctor 0x411580…0x411bc0, field init at ore_tuba.c:11634ff,
pointer setup in 0x412fe0 at ore_tuba.c:13013).

| global | = | class / vtable | what it is |
|---|---|---|---|
| DAT_0050324c | app+0x9c0 | "Screen" (no vtable, plain struct) | display + object registry: DirectDraw/GDI presenter, screen-level view list, lists of all images/surfaces, views, sprites (owns them; frees by tag) |
| DAT_00503214 | app+0x19b8 | task scheduler, vt 0x4edd34 | **user task list**: path animators, bg effects, LAG anims, transitions … ticked once per frame |
| DAT_00503218 | app+0x19a8 | task scheduler, vt 0x4edd34 | system task list (ticked every loop iteration, before the frame gate) |
| DAT_00503224 | app+0x19cc | name registry, vt 0x4edd0c | **transition** factory: `cross`, `?`(0x4fc640), `scroll`, `?`(0x4fc648), `mosaic`, `rotaxis`, `trans`, `?`(0x4fc658) (registered in 0x411f50) |
| DAT_00503220 | app+0x19f0 | name registry, vt 0x4edce4 | **bg-effect** factory: `bg_zoom`, `bg_rot`, `bg_rast`, `bg_haze`, `bg_blur`, `bg_anime` |
| DAT_00503238 = DAT_00503878 | app+0x194 | image loader manager, vt 0x4ef30c | format plugins (`ELG format` vt 0x4eddb4, `Bitmap format` vt 0x4edd7c) + decoded-image cache |
| DAT_00503874 | (tvpgl wrapper, app+0x1900) | 0x4654e0 | non-NULL when tvpgl.dll loaded → selects tvpgl code paths |
| DAT_00503f34 | app+0x168 | archive/file manager | path resolution (`vt+0x44`) for the cache key |

### 1.1 Screen (app+0x9c0, `S`)

| offset | meaning |
|---|---|
| S+0x0c | IDirectDraw-ish object (vt+0x44/+0x58 used when +0x125 set) |
| S+0x70, +0x80, +0x124, +0x125, +0x130 | display mode flags (window/fullscreen/flip) |
| S+0xf8 | **view list** (render list): `{?, head}`; node 0x14 bytes `{obj, prev, next, u32 prio, u8 flag}`, sorted **descending** prio (insert 0x4368f0: before first node with `prio < new`) |
| S+0x104 | HWND; S+0x108 primary surface (DD); S+0x10c back buffer (DD surface); S+0x114..0x120 client rect |
| S+0x110 | "main object" (GDI path, set by N_ee) — presented directly when non-NULL |
| S+0x13c, +0x140 | pixel-format descriptors; S+0x19d..0x19f clear colour (0x437fb0) |
| S+0x15c, +0x160..+0x16c | "has full-screen dest rect" flag + rect (x,y,w,h) (N_ce) |
| S+0x19c | byte: present uses the +0x160 rect (N_cd returns it) |
| S+0x1a4 | current **image/view tag** (N_c5) |
| S+0x1ac / +0x1b0 | list of images & surfaces (circular, node `{next,prev,obj}`) / count |
| S+0x1b8 / +0x1bc | list of view objects (N_106) / count |
| S+0x1cc | current **sprite tag** (N_191) |
| S+0x1d4 / +0x1d8 | list of layer objects (sprites, colour rects) / count |
| S+0x1dc | critical section used around presenting |

All objects created through natives are owned by one of these lists; `N_ef`, `N_193`,
`N_c6`, `N_192` delete them (delete = `vt+0(1)`; layer dtor unlinks itself from its
compositor list and dirties its old rect).

### 1.2 Task scheduler (vt 0x4edd34, DAT_00503214 / DAT_00503218)

Layout: `+8` list head (circular, node `{next, prev, Task*}`), `+0xc` count.

Task base (all tasks): `+0 vt`, `+4 id` (= own address), `+8 next-fire time (ms)`,
`+0xc interval`, `+0x10 remaining count (0 = infinite)`, `+0x14 u8 killed`,
`+0x18 pause counter`, `+0x1c time when paused`.
Task vtable: `+0 dtor`, `+4 typeId()`, `+8 update(now) → nonzero = finished`,
`+0xc finish()` (jump to end state), `+0x1c pause(now)`, `+0x20 resume(now)` (shifts
`+8` and private start time by the paused duration).

Scheduler vtable:

| slot | fn | behaviour |
|---|---|---|
| +04 | 0x47e070 | `add(task, interval, count)` → 0 ok, 1 null, 2 already present |
| +08 | 0x47e120 | `kill(task)`: if present & not killed: killed=1, `task->finish()`; returns 1 if found |
| +0c | 0x47e170 | `killType(typeId)` same for every task with `typeId()==id` |
| +10 | 0x47e3a0 | kill + delete all |
| +14 | 0x47e1c0 | `contains(task)` (ignores killed flag) |
| +18 | 0x47e1f0 | `delay(task, ms)`: next = now+ms |
| +1c / +20 | 0x47e240 / 0x47e290 | `pause(task)` / `pauseType(type)` (type<0 = all) |
| +24 / +28 | 0x47e2f0 / 0x47e340 | `resume(task)` / `resumeType(type)` |
| +2c | 0x47e410 | tick with catch-up loop |
| +30 | 0x47e4f0 | **tick(now)**: for each task: if killed → delete; else if not paused and `now >= next`: `update(now)`; nonzero → delete; else count-- (delete at 0), `next += interval` (if that is still ≤ now, `next = now+interval`) |

Observed typeIds (`N_d1/N_d3/N_d5` arguments): 12000 path animator without target,
**12003 path animator on a layer in a main list, 12004 … in an overlay list**
(0x497360), 12001/12005/12006 second animator class (0x497440, native N_35 – not mine),
13000/13002 LAG-animation tasks (vt 0x4ede98/0x4edec4, natives ~N_1fb), 14000 bg_* effects
(vt 0x4ee0dc…0x4ee234), 15000 bg_anime (vt 0x4ef918), 14001/14003 tasks in 0x402f–0x404b
(vt 0x4ed498, 0x4ed5c0… – text/message side, (guess)), 10003 (vt 0x4ee644).
Scripts use: `N_d1(12003)` ×20 (finish all sprite animations), 14000 ×8, 12004/12005/14001/14003 ×2–3.

---------------------------------------------------------------------------------------

## 2. Object model

```
Image (type 4)                         0x80 bytes, vt 0x4ee290 (base 0x4b6080, vt 0x4f0098)
Surface objects (types 7, 9, 10)       0xd8/0xd0 bytes: header + ISurface sub-object at +0x30
  └─ ISurface (+0x30)                  vt 0x4f0ad4 family; Image embedded at sub+4 (= obj+0x34)
       ├─ Compositor  (sub+0x84)        0x98 bytes, vt 0x4ee0d0  ("layer canvas")
       └─ overlay list (sub+0x88)       0x10 bytes, vt 0x4ee0cc  (second layer list)
Layer objects (live in compositor lists)
  ├─ LayerBase                         vt 0x4ee068 (ctor 0x43a040)
  ├─ Sprite                            0xc0 bytes, vt 0x4ee2f0 (ctor 0x446570)
  └─ ColorRect                         0x40 bytes, vt 0x4ee358 (ctor 0x448210)
View (screen-level blitter)            0x3c bytes, vt 0x4ef024 (ctor 0x4b2930)
PathAnimator (task)                    0x68 bytes, vt 0x4efbac (ctor 0x496080)
```

"Type" = `vt+4` of the outer object (`N_d6`, `N_f7`, `N_18d`, `N_194`… switch on it):
`0` (DS-movie/other, has +0x30 DD surface; only N_f7 handles it), `4` Image,
`7` screen surface (N_de), `8` (vt 0x4f0a9c, ctor 0x4cf4b0, ISurface at **+0x54**; not
created by any used native), `9` N_ea surface, `10` N_eb surface. Natives resolve an
argument to a *source image* as: type 4 → itself; 7/9/10 → `(obj+0x30)->vt+8()` (=obj+0x34);
8 → `(obj+0x54)->vt+8()`; and to an *ISurface* as obj+0x30 (7/9/10) or obj+0x54 (8).

### 2.1 Image (vt 0x4ee290; base struct 0x4b6080)

| off | meaning |
|---|---|
| +04 / +08 | width / height |
| +0c | pitch = `((bpp/8)*w + 3) & ~3` (rows top-down) |
| +10 | colour key: `0xFFFFFFFF` = none (default). 32bpp: raw pixel value compared incl. alpha byte; 8bpp: index |
| +14..+20 | clip rect (l,t,r,b), = full image after create |
| +24 / +28 | **ELG offset X / Y** (type-1 header) — "user" ints, vt+0x44/+0x48 get, vt+0x4c/+0x50 set |
| +2c | owner **tag** (vt+0x54 set / vt+0x58 get) = S+0x1a4 at creation |
| +30 / +34 | GDI HDC / HBITMAP when created as DIB section (flags bit0) |
| +38 | raw allocation; +3c **pixel pointer** (8-byte aligned) |
| +40 | u8 bpp: 8 or 32 (`' '`) only (16/24 refused by 0x43f830) |
| +44 | 8bpp palette (256 × BGRA) |
| +48 | allocation size |
| +50 | u8 **has alpha** (1 when decoded from a 32bpp ELG) |
| +51 | u8 "contents valid/changed" |
| +54 / +58 | dirty-region object (0x24 bytes, 0x4bb810) when flags bit1 |

vtable: +08 width, +0c height, +10 pitch, +14 getFullRect, +18 bits, +1c rowPtr(y),
+20 setColorKey(r,g,b) (32bpp key = `r<<16|g<<8|b`, alpha 0), +24 setColorKeyFromPixel(x,y),
+28 getColorKey, +2c clearColorKey, +34 setClip(rect|NULL), +38 getClip, +3c clipPtr,
+40 clipRectToClip(rect)→bool, +44/+48 get +24/+28, +4c/+50 set them, +54/+58 tag,
+5c resize(w,h).

Create: `0x43f830(w, h, bpp, flags)`: bpp ∈ {8,32} else error 1; flags bit0 = GDI DIB
section (`CreateDIBSection`, top-down: biHeight = -h, 8bpp gets a grey ramp palette),
bit1 = attach dirty region. **Non-DIB pixel memory is not cleared** (operator new).

Pixel format: 32bpp little-endian **B,G,R,A** (`0xAARRGGBB` as u32). Alpha is
straight (non-premultiplied).

### 2.2 Image loading / ELG

`N_e4` → 0x470440 → 0x43f9c0 (target 32bpp); `N_e5` → 0x43fcc0 (keep 8bpp + palette).
1. If the loader cache is enabled (`loader+0x824` budget ≠ 0; budget from 0x48e260:
   explicit MB or by RAM: ≤96MB→0, ≤128→10, ≤192→20, ≤256→30, ≤512→64, else 80 MB):
   resolve the name with the file manager, look it up (0x48e5d0: lower-cased name,
   hash `h=((h+c)*0x401) ^ (h>>6)` per char, final `(h*9>>11 ^ h*9)*0x8001`, 64 buckets,
   LRU list, refcounted). A hit is a decoded DIB (`BITMAPINFOHEADER` + bits); it is
   converted into a fresh image (32-bit source → `+0x50=1`; 24-bit → expanded); offsets
   copied from the entry.
2. Otherwise the ELG plugin (vt 0x4eddb4, decoder 0x4861c0) decodes straight into an image
   created by callback 0x43f920 (which sets `image+0x50 = (src bpp == 32)`), and writes
   the header offsets to `image+0x24/+0x28`.

ELG header as accepted by this EXE (0x4861c0/0x485e90):
```
"ELG" u8 t
t == 1 : u8 bpp, i16 offX, i16 offY, u16 w, u16 h   (13-byte header) -> offsets
t in {8,24,32}: bpp = t, u16 w, u16 h                (8-byte header), offsets 0
anything else (incl. t==2) -> exception 3
```
Payload compression = GARbro `ImageELG.cs` (`UnpackIndexed`/`UnpackRGB`/`UnpackRGBA`+
`UnpackAlpha`); decoder funcs 0x4c0520 (palette/indexed), 0x4c21c0 (RGB → BGRA with
**alpha byte 0**), 0x4c2400 (alpha plane). 8bpp → 32bpp target is palette-expanded
(0x4c2000). Measured in this game's archives: CHR 985×(t1,32bpp) + 4×(t0,32);
PIC 756×(t0,24bpp) (710 are 800×600; also 800×1200, 1200×600, 800×900, 533..538×1205,
264/266×600) + 7×(t1,32); SYS 479×24bpp, 159×32bpp, 121×8bpp (rules, noise), 340×t1.
**No type-2 ELG exists** in PIC/CHR/SYS. The engine never applies the ELG offset by
itself: func_lib reads it with N_f5/N_f6 (character X = `offX - 400 + slotX`, see §6).

Other formats in SYS.LPK (handled by natives outside this range, for orientation):
* `*.gmp` = `"LGM\0"`, u8 len + background ELG name (loaded & set as compositor bg),
  u16 count, then per item u8 len + ELG name + placement record (parser 0x40e330 /
  0x40e8a0, natives N_203/N_208 – UI doc).
* `logo.lag` = `"LAG\0"` animation (parser 0x46ed00, native N_1fb): 12-byte header
  (u16 ?, u16 ?, …, u16 animCount ≠ 0), u32 skip, u32 n + n×16-byte frame records
  (−1 terminated), u32 imageCount, per image `u32 size, u32 offset, i16 x|−1/−2, i16 y`
  + embedded ELG.

### 2.3 Surface objects (types 7 / 9 / 10) and ISurface

Outer object: `+0 vt`, `+0x24/+0x28` user ints, `+0x30 ISurface`.
ISurface (offsets relative to sub = obj+0x30):

| sub+ | meaning |
|---|---|
| +04 | embedded **Image** (the surface's pixels; obj+0x34) |
| +55 | u8 "changed" |
| +58 / +5c | dirty region / active dirty region (DD path) |
| +84 | **Compositor** (layer canvas) bound to the image |
| +88 | overlay layer list (type 7/9 with mode 2) = compositor+0x1c |
| +8c | u8 **use compositor**: 1 when created with mode≠0 (obj+0xbc). 1 → the page is rebuilt by its compositor (invalidations go to the compositor); 0 → the page image is presented as-is and invalidations go to the page's own dirty region (+5c) (0x472140, 0x4bd0c0, 0x4cdbf0) |
| +90 | **linked back surface** (N_f9) |
| +94 | transition work image (same size/bpp, DIB) |
| +98 | active transition object; +9c u8 mode (0/1/2), +9d u8 apply-on-end, +9e u8 call hooks |

ISurface vtable (0x4f0ad4 family): +08 image(sub+4), +0c compositor, +10 overlay list,
+14 linked surface, +18 work image, +1c bpp, +20 get +8c, +24 get +9d, +28 set +8c (ret old; also selects +5c := +58 or 0),
+2c invalidate(rect|NULL), +30 set comp+0x55, +34 set comp+0x56/+0x57,
+38 recreate image(bpp), **+3c swapCompositors(other)** (0x472210), **+40 swapBackgrounds(other)**
(0x4722d0 → 0x43b4c0), +44 free work image, +48 current image (work image while a
transition runs), +4c transition active, +50 get transition, **+54 link(other)** (0x4d13a0:
needs same w,h,bpp → else 1), **+58 startTransition** (0x4d1420), **+5c endTransition(apply)** (0x4d15e0).

Construction (`w,h,bpp,mode`):
* **type 7** `N_de` 0x4cd900 (0xd8 bytes, vt 0x4f091c/0x4f08bc): needs DirectDraw
  (DAT_005040bc); creates a DD off-screen surface at obj+0xd0, image as DIB (+dirty
  region when DAT_004ff321), `mode≠0` → compositor, `mode==2` → also overlay list; clears
  to 0. This is the **screen page** that a View blits to the back buffer.
* **type 9** `N_ea` 0x4bcf20: image flags 3 (DIB + dirty region), mode≠0 → compositor,
  mode==2 → overlay list. Used as screen page in GDI mode (made main with N_ee).
* **type 10** `N_eb` 0x4d16e0: plain memory image (flags 0), mode≠0 → compositor (never an
  overlay list). Used as **back page** (`G[896]{+4}`) linked with N_f9.

### 2.4 Compositor ("layer canvas", 0x98 bytes, vt 0x4ee0d0, ctor 0x43a860)

| off | meaning |
|---|---|
| +08 | target image (sub+4) (0x43a980) |
| +0c | **main layer list** object (vt 0x4ee0cc): +0x10 head, +0x14 tail, +0x18 owner (=compositor) |
| +1c | **overlay layer list** (sub+0x88) or NULL |
| +20 | dirty region (0x4bb810; list of rects, +0x24 first, +0x2c count) |
| +44 | background image (any image; NULL = colour) |
| +48 / +4c | background **source offset** (the visible window starts at (x,y) in the bg image) |
| +50 | bg colour (32bpp target: u32 `0x00RRGGBB`) ; +52 = low byte used for 8bpp targets |
| +51 | ? (returned by N_1a3) |
| +54 | u8 compose disabled |
| +55 | u8 no background (skip bg fill/copy) |
| +56 / +57 | u8 skip main list / skip overlay list |
| +58 | cached transformed background (zoom/rotate), drawn at offset 0,0 |
| +60 | u8 filter flags for bg transforms (init 1) |
| +64..+94 | cache keys for +58 (x,y,w,h,src rect / angle(+84), zoom double(+88)), +90/+94 rotation centre (= bg w/2,h/2 by default) |

Layer list node (0x24 bytes): `+0 obj, +4..+10 last drawn rect (+4 = −1 none),
+14 u32 priority, +18 tag, +1c prev, +20 next`; `obj+0x18` = list it is in.
Insert (0x43a150): before the first node with `prio >= new` → list ascending by priority,
new node goes **in front of equal priorities**; 0x43a240: after equals.

### 2.5 Layer objects

LayerBase (vt 0x4ee068):

| off | meaning |
|---|---|
| +04 | **tag** (= S+0x1cc at creation; filter key for N_192/N_1a8) |
| +08 | u8 visible (ctor: 1) |
| +09 | u8 dirty |
| +0c / +10 | x, y (position on the surface) |
| +14 | u8 blend mode (default 0) |
| +15 | u8 opacity (default 255) |
| +18 | owning layer list |

Common vtable: +04 visible, +08 setVisible(b) (→ dirty), +0c isDirty, +10 setDirty(b),
+14 x, +18 y, +1c mode, +20 opacity, **+30 setMode, +34 setOpacity, +38 setPos(x,y),
+3c move(dx,dy)**, +40 setSize(w,h), +44 setSize+setPos, +48 invalidate(subrect),
+4c hitTest(x,y,usePixels), +50 intersects(obj), +54 getBounds(rect), +58/+5c draw(dst, outRect),
+60 **drawClipped(dst, clip)**, +64 reset.

**Sprite** (0xc0, vt 0x4ee2f0) – shows (part of) an image:

| off | meaning |
|---|---|
| +1c | source image |
| +20 | cached scaled/rotated image |
| +24..+30 | source rect l,t,r,b |
| +34..+40 | **dest rect** l,t,r,b on the surface (= bounds) |
| +44 / +48 | source scroll offset (N_1c1, path mode 2) |
| +4c / +50 | zoom target size (0 = none) |
| +54 | rotation angle (units: 512 = 360°, table at loader+0xa04) |
| +58 | double zoom (default 1.0) |
| +60 / +64 | rotation centre (default w/2, h/2) |
| +68 / +6c / +70 / +74 | rotated bbox offset / size (0 = not rotated) |
| +78 | u8 filter flags (low nibble < 4, high nibble ≤ 1) |
| +80.. | 4×16 bytes transform workspace |

* `0x446690 init(image, rect|NULL, x, y)`: `reset()` (mode 0, opacity 255, no zoom/rot),
  src rect = rect (clipped to image) or full image; **if image+0x50 (has alpha) → mode 2**;
  pos = (x,y); dest = (x, y, x+srcW, y+srcH); centre = size/2; dirty.
* `0x446a20` recompute dest: unrotated: `l=x, t=y, r=x+(zoomW or srcW), b=y+(zoomH or srcH)`.
* setMode (0x4469f0) accepts 0..31; setOpacity 0x46cf10; setPos 0x446ac0; move 0x446af0.
* hitTest (0x4477a0): inside dest rect; with `usePixels`: 32bpp no-key → `alpha != 0`;
  keyed → `pixel != key`; 8bpp no-key → true.

Sprite draw (0x447bc0 clipped / 0x4478f0 full), `src = zoom/rot cache or +1c`:
```
if src.bpp == 32 and src.colorKey == -1:      # the normal case
    tvpgl[+0x24](dst, src, destL, destT, srcRect, mode=+14, opacity=+15, hda=0)
elif src.bpp == 32 (keyed):
    mode 0: opa==255 ? keyed copy 0x441df0 : keyed const-alpha 0x444460
    mode 2: opa==255 ? 0x442df0 : 0x4b7a40      mode 4: 0x444f10(..,1) / 0x4b7690
    mode 5: 0x444f10(..,2) / 0x4b7890            other: not drawn
else (8bpp source): 0x441df0 = opaque copy through the palette (index==key skipped);
    mode and opacity are ignored
```
Layers with `visible==0` or `opacity==0` are skipped by the compositor.

**ColorRect** (0x40, vt 0x4ee358) – solid rectangle: `+1c` RGB (`B=+1c,G=+1d,R=+1e`),
`+15` opacity (= colour>>24 at creation), `+20/+24` w,h, `+28/+2c` size override (vt+40),
`+30..+3c` rect (x,y,x+w,y+h), `+14` mode 0..3 (vt+30 refuses >3).
Draw (0x448740/0x448940): mode 0 → `0x4b6ea0` = tvpgl[+0x10] constant-colour alpha
fill (`opacity==255` → plain fill); mode 2 → additive fill 0x4b7080 with colour channels
premultiplied `tbl[a*256+c]` (DAT_00524fc0, ≈ c·a/255); mode 3 → subtractive 0x4b7220
(same premultiply); mode 1 → not drawn.

**View** (0x3c, vt 0x4ef024) – lives in the Screen view list, blits a type-7 surface to
the DD back buffer: `+08 visible`, `+10 surface`, `+14..+20 source rect`, `+24/+28` source
scroll, `+2c/+30` **dest x,y**, `+34/+38` dest size (0 = 1:1; else stretched Blt).
vt+0x34 draw (0x4d2080): composes the surface (0x4cdbf0) then BltFast/Blt to the back buffer.

---------------------------------------------------------------------------------------

## 3. Frame composition

### 3.1 Main loop (0x401a90 → 0x4016b0 / 0x401920)

Per iteration: system scheduler (app+0x19a8) `tick(now)`; Windows message pump;
when the 60 fps frame gate (app+0x1948, `0x4388d0(...,60)`) opens:
mouse/keyboard update (0x436220, 0x435ea0); optional back-buffer clear (0x437fb0, only if
app+0x1ab4); **user scheduler `tick(now)`** (app+0x19b8: animations, effects, transitions);
**render the view list** (0x436db0: for each node head→tail, i.e. *descending* prio,
`view->vt+0x34(backBuffer, clientRect, screen)`); return to the script runner
(0x4252d0 runs the SOB VM until it yields with `end`). The next call first **presents**
(0x438270: Flip/Blt or GDI BitBlt of the back buffer; or 0x437ff0 when S+0x110 is set:
render the main object straight to the window). So: present(prev) → tasks → render →
script slice. Timebase: `GetTickCount` or `timeGetTime` (DAT_00503869).

### 3.2 What is on screen

* gameinit.sob: `G[896]{+0} = N_ed()`; if 0 (normal DirectDraw mode):
  `G[896]{+0} = N_de(800,600,32,2)` (screen page, compositor + overlay list),
  `G[897] = N_106(G[896]{+0}, 0xffff)` (full-size view at 0,0),
  `N_10a(G[897], 10000, 255)`. Then always `G[896]{+4} = N_eb(800,600,32,1)` (back page) and
  `N_f9(G[896]{+0}, G[896]{+4})` (link for transitions), `G[899] = N_e3(800,600,32,0)`.
* Sub-screens (config/saveload/history/gallery…) create their own page: DD mode →
  `N_10f(G[897],0)`, `N_de(800,600,32,2)`, `N_106(…)`, `N_10a(view, 9999, 0)`;
  GDI mode → `N_ea(800,600,32,2)` + `N_ee(page)`; on exit func_lib 0x21d40 re-shows
  `G[897]` (`N_10f(G[897],1)`) or restores `N_ee(G[896]{+0})`.

For the reimplementation: **frame = black 800×600, then every visible View in
descending priority: blit its surface's *current image* (work image while a transition
runs, else the composed page) at (dest x,y)**. In practice exactly one full-screen view
is visible, so the frame is the composed screen page.

### 3.3 Compositing a page (0x43b800 / 0x43c590 / 0x43bc20)

Invalidation: `0x43b660` walks both lists; for each layer with `isDirty`:
add its previous rect (node+4) to the region, fetch `getBounds`, clear dirty,
clip to the target, add the new rect and remember it. `0x43b390` = invalidate all
(marks every visible layer dirty and adds the full target rect). Background setters,
N_1a4, transitions ending, N_ee etc. call 0x43b390. The dirty region (0x4bb810/0x4bbc40)
is an optimisation only — a reimplementation may redraw the full page every frame.

For each dirty rect `R` (in the page's own coordinates):
```
# 1. background (skipped if comp+0x55)
bg = comp+0x58 (cached transform, offset 0,0) or comp+0x44 (offset +48,+4c)
if bg:  opaque copy (0x441df0) of bg source rect R+offset → R   # no blending, alpha copied
        # parts of R outside bg are left UNCHANGED (only the zoom path pre-fills colour)
else:   fill R with comp+0x50 (32bpp; alpha byte 0) / comp+0x52 (8bpp)
# 2. main list from TAIL (highest prio) to HEAD (lowest prio)   (skipped if comp+0x56)
for node in tail..head: if obj.visible and obj.opacity: obj.drawClipped(target, R)
# 3. overlay list, same order                                    (skipped if comp+0x57)
```
⇒ **smaller priority value = drawn later = in front**; overlay-list layers are always in
front of all main-list layers; among equal priorities the most recently inserted (0x43a150)
is in front. DAT_004fc479 (N_1ab) only switches to drawing in horizontal bands
(8–128 px high depending on width) – same result.

The DD page (type 7) then copies the dirty rects into its DirectDraw surface (0x43c590,
pixel-format conversion); the view blits that surface. N_1a8 (0x43bc20) runs the same
algorithm over the full page into an arbitrary same-size image, optionally filtering
layers by tag (mode 1: `obj+4 == tag`, 2: `!=`), optionally including the overlay list.

### 3.4 Transitions (interface only; algorithms in the transition doc)

`N_fb(page, name, mode, applyOnEnd, args…)`: transition object from DAT_00503224
(`vt+0x24(isurface, name, &args[4])`), then `page.isurface->vt+0x58(t, mode, apply, hooks)`
(0x4d1420): requires a compositor and a linked surface (+0x90); ends a running one first;
allocates the work image (+0x94); stores mode/apply/hooks; hooks (`!app+0xe37`) call
DAT_0050386c / DAT_00503870 at start/end (callbacks 0x414000/0x414090).
While active the page draws via 0x4cee60 / 0x4ce2b0 / 0x4ce860 (by mode 0/1/2) and the
visible image is the work image. End (`vt+0x5c(apply)`, 0x4d15e0): release the transition,
if `apply`: mode 1 → **swapCompositors(linked)**, mode 2 → **swapBackgrounds(linked)**,
then full invalidate.
* swapCompositors (0x472210): the two pages exchange their main compositors (layer
  list + background) and each is re-bound to its own image; the overlay lists stay
  with their pages. Net effect: what was built on the back page (G[896]{+4}) is now shown on
  the screen page and vice versa.
* swapBackgrounds (0x43b4c0): exchange only bg image/offset/cache/colour/transform state.

### 3.5 tvpgl.dll

Loaded by 0x4654e0 (`tvpgl.dll` from the game dir). Exports only `tvpglConfig`,
`tvpglConfig2`, `GetLibrary` – **no per-function exports**. The EXE calls
`tvpglConfig(0, 0x45f780, 0x4b58a0, 0x4b5e10)` (MMX variants 0x45f940/0x4b5aa0/0x4b5f00 if
app+0x8f) – these become the DLL's opaque-copy callbacks (1004d20c/…204/…208), then
`lib = GetLibrary(cpuFlags)` (object 0x3ac bytes, filled by tvpgl:0x10001000) and copies
the table into globals:

| lib+ | EXE global | tvpgl fn | role / used by |
|---|---|---|---|
| +04 | DAT_00503b78 | &tvpgl:0x1004d1f0 | **rect written by the last op** (EXE adds it to dirty regions) |
| +08 | DAT_0050394c | 0x10001d20 | fill32(img, value, rect): plain u32 store (`0x00RRGGBB` → alpha 0) |
| +0c | DAT_00503c6c | 0x10001e70 | fill8(img, index, rect) |
| +10 | DAT_00503a3c | 0x10001fc0 | constColorAlphaFill(img, rgb, opa, rect): opa 255 → TVP FillColor, else TVP ConstColorAlphaBlend (ColorRect mode 0) |
| +14 | DAT_00503ab0 | 0x10002130 | colour fill variants with dest-alpha handling |
| +1c | DAT_005039d8 | 0x10002440 | fill mask/alpha |
| +24 | DAT_00503a9c | 0x10002a50 | **blit(dst, src, x, y, srcRect, mode, opa, hda)** – sprites |
| +28 | DAT_005039ac | 0x10005af0 | stretch blit(dst, src, &dstSize|NULL, x, y, srcRect, mode, flags, opa, hda) – sprite zoom, bg zoom |
| +2c | DAT_00503bd0 | 0x10007800 | affine/rotate blit – sprite & bg rotation |
| +30..+68 | various | 0x1000ac60… | used only by wrappers 0x495850…0x495f70 (image-effect natives) and transitions |
| +6c… | (TVPGL raw pointers) | | only +0x174 (DAT_0050398c, TVPGL #67) used by 0x45e1a0 |

`blit` (tvpgl:0x10002a50): clip `srcRect` to the source (shifting x,y), then the dest
span to the **destination image size** (not its clip rect); `mode==0 && opa==255` → opaque
32-bit copy via the EXE callback (0x45f780: copies all 4 bytes incl. alpha; `hda` → keep
dest alpha); `opa==0` → nothing; otherwise per row `jumptable[mode]` (0x10004c24, 32
entries; mode 13 = no-op). Each mode picks one of up to 4 TVPGL kernels:
(opa==255,!hda), (opa==255,hda), (opa<255,!hda), (opa<255,hda).

Kernels for the modes this game uses (C reference versions decompiled; the MMX/SSE
versions installed by tvpgl:0x1002cf00 compute the same thing). All arithmetic is
on packed u32 with wrap-around, exactly as written:
```
// mode 2, opa==255  (tvpgl 0x1000f4e0, "TVPAlphaBlend")
a  = s >> 24
rb = ((((s & 0xff00ff) - (d & 0xff00ff)) * a >> 8) + (d & 0xff00ff)) & 0xff00ff
g  = ((((s & 0x00ff00) - (d & 0x00ff00)) * a >> 8) + (d & 0x00ff00)) & 0x00ff00
d' = rb + g                       // dest alpha := 0   (HDA variant 0x1000f6b0 keeps d & 0xff000000)
// mode 2, opa<255   (0x1000f8a0, "TVPAlphaBlend_o"): a = ((s>>24) * opa) >> 8, rest identical
// mode 0, opa<255   (0x10014a80, "TVPConstAlphaBlend"): a = opa (constant), same formula, alpha := 0
//                    (HDA variant 0x10014c40 keeps dest alpha)
// mode 0, opa==255  : straight copy of the 32-bit pixel (alpha copied)
```
Note the `>>8` (not /255): with a=255, s=255, d=0 the result is `(255*255)>>8 = 254`, so
fully opaque alpha pixels are *not* exactly the source colour (error ≤ 1 per channel).
Only the C reference kernels were read; the MMX/SSE kernels selected at run time
(tvpgl:0x1002cf00, e.g. 0x1003bae0/0x1003c260 for mode 2) are assumed to give the same
result (Kirikiri convention) – unverified.

Jump-table → kernel globals (tvpgl data addresses; `—` = only 2 variants):

| mode | opa255 | opa255+hda | opa<255 | opa<255+hda |
|---|---|---|---|---|
| 0 | copy | copy(hda) | 1008d6b8 ConstAlpha | 1009de68 ConstAlpha_HDA |
| 1 | 1007d678 | — | 1008d68c | — |
| 2 | 1008dda8 Alpha | 1008d6f4 Alpha_HDA | 1006d610 Alpha_o | 100bdf28 Alpha_HDA_o |
| 3 | 1006d608 | — | 100ade80 | — |
| 4 | 1008db48 | 100ade84 | 1008ddd8 | 1005d594 |
| 5..12, 14..31 | see table 0x10004c24 (dumped in the scratch analysis) | | | |

Mode 3 composes straight-alpha images while retaining the output alpha. The earlier
guess that it consumed premultiplied additive alpha was incorrect: the 1.01 EXE's
N_16a implementation at 0x4b8040 calls DLL mode 3; its C fallback at 0x4692a0 reads
both source and destination alpha, selects a colour weight from 0x5048c0, and writes
the combined coverage from 0x5148c0. The port uses direct source-over arithmetic;
exact rounding of those runtime-generated lookup tables is not established.
Other modes remain incompletely mapped. **Scripts only ever set sprite mode 0 or 2** (`N_1b0(obj,2)`,
`N_1b0(uiobj,0)`); 32bpp ELG sprites get mode 2 automatically.

`stretch` (tvpgl:0x10005af0): if dest size == src size → `blit`. Negative sizes flip.
Filter = `flags & 0xf`: 2 or 3 with opa 255, mode 0, fully inside → interpolating path
(tvpgl:0x10004cb0, guess: bilinear); otherwise nearest neighbour in 16.16 fixed point
(`step = (srcW<<16)/dstW`). Sprite zoom uses sprite+0x78 (default 0 → nearest); bg zoom
uses comp+0x60 (default 1 → nearest).

---------------------------------------------------------------------------------------

## 4. Path animator (N_1b8 / N_1b9)

0x68-byte task, vt 0x4efbac (update 0x4961a0, finish 0x4962e0/0x496af0, interp 0x496c80).

| off | meaning |
|---|---|
| +24 | target layer object |
| +29 | u8 animate opacity (always 1) |
| +2b | u8 call start/end hooks |
| +2c | **ms per segment** |
| +30 | initial delay (ms) |
| +38 | double easing exponent `e` |
| +40 | key count n+1 (key 0 = state at start) |
| +44 | keys: n+1 × (double x, double y) |
| +48 | keys: n+1 × u8 opacity |
| +4c | u8 "temporarily visible": `setVisible(1)` when the delay expires, `setVisible(0)` at the end |
| +4d | u8 what moves: 0 = opacity only, **1 = position (vt+0x38)**, 2 = sprite source offset (N_1c1; scrolling a large CG inside its sprite) |
| +4e | u8 spline (natural cubic) instead of linear |
| +4f | u8 loop |
| +54 / +58 | start time / total = (n)·segment |
| +5c / +60 | spline coefficients x / y |

Start (0x4969a0): key0 = (`vt+0x14` x, `vt+0x18` y) or the sprite source offset
(+44,+48) for mode 2, opacity0 = `vt+0x20`; then `scheduler.add(this, 0, 0)`; returns the
task (handle for N_cf/N_d0).

Update(now): first call sets start; wait for the delay (then optional show);
`t = now - start`; if `t >= total`: loop → restart, else finish (set last key exactly,
optional hide) and return 1. Easing with `T=total, e=+38`:
`e>0: t' = T·(t/T)^e`;  `e<0: t' = T·(1-(1-t/T)^(-e))`;  `e==0: t' = t` (truncate to int).
Segment `i = t'/seg`, `f = (t' % seg)/seg`:
* linear: `x = trunc(Xi + (Xi+1 - Xi)·f)` (same for y)
* spline: `x = trunc(Xi + f·((Xi+1-Xi) - (2Mi + Mi+1) + f·(3Mi + f·(Mi+1 - Mi))))`
  with `M` solving `M(k-1) + 4M(k) + M(k+1) = d(k+1) - d(k)`, `d(k)=X(k)-X(k-1)`,
  `M0 = Mn = 0` (Thomas algorithm 0x4965a0)
* opacity: `A_i + trunc((A_i+1 - A_i)·f)` → `setOpacity` (always linear)

---------------------------------------------------------------------------------------

## 5. Natives

Return value "obj/0" = new object pointer or 0 on failure.

### 5.1 Screen, images, surfaces (N_c5..N_ef)

| native | addr | args | behaviour |
|---|---|---|---|
| N_c5 | 0x426370 | tag | `old=S+0x1a4; S+0x1a4=tag; return old`. Tag stored in new images/surfaces/views. gameinit: `N_c5(-1)` |
| N_c6 | 0x426390 | tag, flags | 0x471510: delete views with `tag` (flags&2: with tag ≠ given); if flags&1 also images/surfaces (tag = `vt+0x58`). ret 0 |
| N_c8 | 0x4263e0 | name | loader cache: find entry by name, drop one reference (delete at 0). (no meaningful ret) |
| N_cd | 0x426490 | – | return S+0x19c (present uses full-screen rect) |
| N_ce | 0x4264a0 | &x,&y,&w,&h | if S+0x15c: write S+0x160..0x16c (NULL ptrs skipped), return 1; else 0 |
| N_cf | 0x426520 | task | scheduler `contains` → 0/1 |
| N_d0 | 0x426540 | &task | `t=*p; *p=0; scheduler.kill(t)` (task jumps to its end state, deleted next tick) → 0/1 |
| N_d1 | 0x426560 | typeId | kill all tasks of that type (see §1.2) |
| N_d2 / N_d3 | 0x426580 / 0x4265a0 | task / typeId | pause (scripts: `N_d3(14003)`) |
| N_d4 / N_d5 | 0x4265c0 / 0x4265e0 | task / typeId | resume (`N_d5(14003)`) |
| N_d6 | 0x426600 | target, effectName, params… | bg effect: `e = DAT_00503220.create(name)`; if app+0xe37 and `e+0x24==1` → `e+0x24=0`; `e->vt+0x34(DAT_00503214, isurface(target), &args[2])`; returns e (task) or 0. Effects: bg_zoom/rot/rast/haze/blur/anime (effect doc) |
| N_d7 / N_d8 | 0x4266c0 / 0x4266e0 | effect, params… | `effect->vt+0x3c(&args[1])` / `vt+0x44(&args[1])` (parameter change; −1 if null) |
| N_d9 | 0x426700 | effectName, params… | call the effect type's static helper (4th registry field, e.g. bg_zoom → 0x43d010) with `&args[1]`; −1 if unknown |
| N_de | 0x4267e0 | w,h,bpp,mode | new type-7 screen page (§2.3) → obj/0. `N_de(800,600,32,2)` |
| N_e3 | 0x4268d0 | w,h,bpp,flags | new Image (`bpp` 8/32; flags bit0 DIB, bit1 dirty region) → obj/0. `N_e3(800,600,32,0)` |
| N_e4 | 0x426900 | filename, flags | load image as 32bpp (§2.2) → obj/0 |
| N_e5 | 0x426930 | filename, flags | load keeping 8bpp+palette (rule/mask images) → obj/0 |
| N_e8 | 0x4269c0 | text, font|0 | render a text string into a new 32bpp image (0x49dd40; font object or default DAT_00503fb0) – text doc |
| N_e9 | 0x4269f0 | srcImage, flags | duplicate image (flags 2 = same DIB/dirty flags as source); copies pixels, palette, key, offsets, has-alpha |
| N_ea | 0x426a10 | w,h,bpp,mode | new type-9 surface → obj/0 |
| N_eb | 0x426a40 | w,h,bpp,mode | new type-10 surface (back page) → obj/0. `N_eb(800,600,32,1)` |
| N_ed | 0x426aa0 | – | return S+0x110 (main object, 0 in DD mode) |
| N_ee | 0x426ab0 | surface | S+0x110 = surface; `isurface->vt+0x2c(NULL)` (full invalidate) under S+0x1dc lock |
| N_ef | 0x426ad0 | &obj | delete image/surface from S+0x1ac and zero the variable (left unchanged if not found). 103 uses |

### 5.2 Image/surface queries and ops (N_f0..N_10f)

| native | addr | args | behaviour |
|---|---|---|---|
| N_f0 / N_f1 | 0x426af0 / 0x426b10 | obj | `vt+8` / `vt+0xc` = width / height (surfaces forward to their image); 0 if null |
| N_f2 | 0x426b30 | obj, x, y | `vt+0x24`: colour key := pixel(x,y) (−1 if outside → none); returns `key == -1` |
| N_f5 / N_f6 | 0x426ba0 / 0x426bc0 | img | ELG offset X / Y (`vt+0x44`/`+0x48`) |
| N_f7 | 0x426be0 | obj, r, g, b | fill whole image with colour (32bpp value `r<<16|g<<8|b`, alpha 0; type 0 → its DD surface); returns **0 on success**, 1 failure, −1 bad type |
| N_f9 | 0x426dd0 | page, back|0 | `isurface(page)->link(isurface(back))` (sizes/bpp must match) → 0 ok / 1 / −1 |
| N_fa | 0x426ec0 | page, mode | with L = linked surface: mode≠1 → `swapCompositors(L)`; mode 1 or 2 → `swapBackgrounds(L)` (mode 2 = both) → 1 |
| N_fb | 0x426f50 | page, name, mode, apply, params… | start transition (§3.4) → 0 ok, −2 unknown name, 1..4 errors, −1 bad obj |
| N_fc | 0x427010 | page | end transition now: `vt+0x5c(vt+0x24())` → 1 |
| N_fd | 0x427090 | page | transition active? |
| N_fe | 0x427120 | page | `isurface->vt+0x18` (transition work image) |
| N_106 | 0x427420 | surface, l,t,r,b | 0xffff, x, y | new View (0x471180) for a type-7 surface, src rect (default full), dest (x,y); tag = S+0x1a4 → obj/0. `N_106(page, 0xffff)` |
| N_10a | 0x4275b0 | obj, prio | −1, flag | insert into the Screen view list (descending prio; prio −1 → 0) |
| N_10f | 0x427670 | obj, bool | `vt+8(bool)` = setVisible (used on views) → 1 |

### 5.3 Sprites and compositor (N_18d..N_1aa)

| native | addr | args | behaviour |
|---|---|---|---|
| N_18d | 0x42f170 | src, l,t,r,b | 0xffff, x, y | **new Sprite** showing `src` (image or surface's image), source rect `SetRect(l,t,r,b)` or full, at (x,y); tag S+0x1cc; added to S+0x1d4 (not yet to a compositor) → obj/0. 7 args (x,y absent when 0xffff → 0,0) |
| N_18e | 0x42f240 | sprite | clone sprite (0x447e70; copies all state, re-creates zoom cache) → obj/0 |
| N_18f | 0x42f280 | w, h, argb, x, y | **new ColorRect** `w×h` at (x,y), RGB = argb&0xffffff, opacity = argb>>24 → obj/0 |
| N_191 | 0x42f2c0 | tag | set sprite tag S+0x1cc, return old |
| N_192 | 0x42f2e0 | tag, invert | delete layer objects with tag (or ≠ tag) |
| N_193 | 0x42f300 | &obj | delete layer object, zero the variable (69 uses) |
| N_194 | 0x42f320 | page, obj, prio, tag, overlay | add layer object to the page's compositor: `prio==-1` → main list, prio 0, tag 0; `overlay==0` → main list (prio, tag); else overlay list (4 if the page has none). → 0 ok, 1 null obj, 2 already in a list, −1 bad page |
| N_199 | 0x42f660 | page, obj, prio, onlyIfChanged | re-sort obj to `prio` (in front of equals) in whichever list holds it → 1 found |
| N_19a | 0x42f740 | same | same but placed behind equals (0x43a440) |
| N_19d | 0x42f970 | page, bg|0, x, y, redraw | set background image (image/surface/…; 0 = none) with source offset (x,y); rotation centre := bg size/2 → 1 |
| N_19e | 0x42fa70 | page, rgb, mode | mode 0/1: remove bg image, colour := rgb, (mode 1 → invalidate); mode ≥2: only change the colour value |
| N_19f | 0x42fb00 | page, x, y | set bg source offset; invalidate → 0 if unchanged |
| N_1a0 | 0x42fb80 | page, x, y, w, h, l,t,r,b | 0xffff | **zoomed background**: bg src rect (default full) stretched to w×h placed at (x,y) into the cache image (page size); area not covered pre-filled with the bg colour; `w,h ≤ 0` → bg size; identity → cache dropped |
| N_1a2 | 0x42fce0 | page, x, y, angle, zoom(float bits) | **rotated/zoomed background** (0x43af80, 32bpp bg only): the bg quad (size·zoom) is rotated about centre·zoom (comp+0x90/94) using the cos table at `[DAT_0050387c+0xa04]` (`tbl[k]=cos(2πk/512)`): `x'=cos·dx+sin·dy, y'=−sin·dx+cos·dy` (angle units 1/512 turn; positive = counter-clockwise on screen, derived), the quad's bounding box is moved to (x,y), drawn into the cache with tvpgl+0x2c (uncovered = bg colour). angle 0 & zoom 1 & x=y=0 → cache dropped |
| N_1a3 | 0x42fd60 | page, &bg,&x,&y,&c52,&c51,&color | read background state |
| N_1a4 | 0x42fde0 | page | full invalidate (0x43b390) |
| N_1a6 | 0x42fef0 | page, bool | `isurface->vt+0x28(bool)`: enable/disable compositor rendering of the page (sub+0x8c), returns old. With 0 the page image is shown untouched |
| N_1a8 | 0x42ffe0 | page, dstImage, mode, tag, withOverlay | snapshot: compose the page (bg + layers filtered by tag: mode 0 all / 1 `==tag` / 2 `!=tag`) into dstImage (same size) → 0 ok, 1 bad dst, 2 size mismatch |
| N_1a9 | 0x4300d0 | page, hideMain, hideOverlay | comp+0x56/+0x57 |
| N_1aa | 0x430150 | page, bool | comp+0x55 (no background) |

### 5.4 Generic layer ops (N_1ab..N_1c7)

| native | addr | args | behaviour |
|---|---|---|---|
| N_1ab | 0x4301c0 | bool | DAT_004fc479 (banded composition; cosmetic) |
| N_1ac | 0x4301e0 | obj, bool | `vt+8` setVisible (107 uses; also used on UI/text objects) |
| N_1ad | 0x430200 | obj | 0 if not in a list, else visible+1 |
| N_1ae | 0x430220 | obj | `vt+0x10(1)` mark dirty → 1 |
| N_1b0 | 0x430290 | obj, n | `vt+0x30(n)`: sprites/rects = set blend mode (−2 null) |
| N_1b1 | 0x4302b0 | obj, n | `vt+0x34(n)`: set opacity 0..255 (94 uses) |
| N_1b2 | 0x431340 | obj, x, y | `vt+0x38`: set position → changed? (64 uses) |
| N_1b3 | 0x4302d0 | obj, dx, dy | `vt+0x3c`: move |
| N_1b4 | 0x430300 | obj, x, y, pixelTest | `vt+0x4c`: hit test |
| N_1b6 | 0x430350 | obj, &l,&t,&r,&b | `vt+0x54` bounds → 1 |
| N_1b8 | 0x430430 | obj, msPerSeg, "(x,y,a)(x,y,a)…", delay, tempVisible, mode|0xffff, spline, "ease", loop, hooks | path animator (§4); `0xffff` → mode 1, linear, ease 0, no loop, hooks 1. "ease" is a string (`atof`). → task/0 |
| N_1b9 | 0x430580 | obj, msPerSeg, count, int[3*count] (x,y,a), delay, tempVisible, mode|0xffff, spline, "ease", loop, hooks | same with an array |
| N_1c1 | 0x430c00 | sprite, sx, sy | set source scroll offset (moves the source rect by the delta) |
| N_1c2 | 0x430c30 | sprite, flags | set filter flags (+78) and rebuild zoom/rot cache |
| N_1c7 | 0x430d80 | sprite, src, l | −1 | 0xffff, t, r, b, x, y | change the sprite's image: `-1` → swap image keeping rect/pos (cache dropped if bpp differs); else full re-init via 0x446690 (**resets mode, opacity, zoom; pos := (x,y)**) |

(The vtable decides what these generic natives mean for UI/text objects; see those docs.)

---------------------------------------------------------------------------------------

## 6. func_lib wrappers used by scenes

Source: lifted func_lib (symbolic lifter output kept in the scratch analysis, not in the
repo). `a0` = first pushed argument. `G[n]{+k}` = int at byte offset k of global array
`G[n]`; page `p` ∈ {0,1} is stored at `{+4p}`; character index `i = slot + 6·p`
(slot 0..5). Almost every wrapper starts with `if (G[905]) return` (G[905] = silent
replay after loading a save, see 6.6) and with **L_19ba8** = "finish pending transition"
(`if G[829] && N_fd(G[896]{+0}) → N_fc`; G[829]=0), so any new command force-completes a
running transition. Beware: scene scripts contain dead calls behind constant guards
(`ge 0xffffffff,0 ; jz` before e.g. `L_17514(slot,-1,…)`).

### 6.1 Page model

* `G[896]{+0}` = **front page** (type 7 in DD mode, presented); `G[896]{+4}` = **back
  page** (type 10), linked by `N_f9`. Scene commands build the next screen on the back
  page and then run a transition on the front page with `swapMode 1` (→ swapCompositors,
  §3.4); at the same moment the script swaps its own per-page state (`L_19b40` →
  `L_28018` bg vars + `L_2928c` char vars). Afterwards page 0 holds the new screen.
  **L_83fc()** (1063 calls) then re-syncs back := front (shares the bg image, clones every
  char sprite with `N_18e` and `N_194`s it into the back page) before the next change.
* Background/CG is **not a layer**: it is the compositor background (`N_19d`, `N_19e`,
  `N_19f`, `N_1a0`, `N_1a2`).
* Characters: sprites at **priority 2000 + z** (z 0..5, 0 = front), tag 100 (G[903]=100
  while creating), main list. Message window frames (advinit) prio 1500, frame buttons
  1100 → in front of characters; system UI 50/100/200; eyecatch 5000/10000.
  Verified: lower priority value is in front.
* Character placement: `x = G[777] − 400 + N_f5(img)`, `y = G[778] + N_f6(img)` where
  G[777] is the logical centre line (400 centre, 200 left, 600 right) and the ELG
  type-1 offset is relative to an 800-wide canvas.

### 6.2 Saved per-page state (contiguous 780-byte block G[757]..G[783], copied to G[784] on every committed text line)

| var | content |
|---|---|
| G[757] | char[2][24] bg name ("BG41B" for numbered BGs, full path "PIC\EF05A.ELG" for raw CGs, "" none) |
| G[758] | char[2][24] bg_anime name |
| G[760] | solid bg colour 0xRRGGBB (when no image) |
| G[761] | bg filter flags (L_1d4bc) |
| G[762] / G[763] | BG number (−1 raw/colour) / variant (0 base, 1 A=evening, 2 B=night, 3 C, 4 D) |
| G[764], G[765] | bg scroll offset (N_19d/N_19f) |
| G[766..769] | bg zoom src rect x,y,w,h (N_1a0); G[766]=−1 none |
| G[770], G[771] | bg rotation angle / zoom (N_1a2; G[771]=0 none) |
| G[772..774] | bg_anime state/params |
| G[780] | "frozen" (L_eff0) |
| G[781], G[782][5] | persistent screen effect (0 none, 1 noise, 2 endless quake, 3 bg_rast, 4/5 bg_haze, 6..9 paper/light/snow/rain) + params |
| G[783] | BGM number |
| G[759] | char[12][24] CHR stem ("KAZ01A") |
| G[775] | char id (from first 3 letters; −1 empty, 99 unknown) |
| G[776] | char colour-filter flags |
| G[777], G[778] | logical x (centre line), y shift |
| G[779] | z 0..5 |

Runtime handles (not saved): G[898]{p} bg Image (may be shared by both pages – always
compare before freeing), G[820]{p} bg effect task, G[821]/G[822] bg_anime task/data,
G[835]{p} freeze snapshot, G[836] quake task, G[823][i] char Image (may be shared),
G[824][i] char Sprite, G[828][i] char motion task, G[829] transition active,
G[830] "swap chars too" (always 1), G[831] current rule image. G[787]/G[788] global /
auto (from bg variant) evening-night tint of characters.

### 6.3 Transition wrappers (front page = `surf0`)

| wrapper (calls) | args | natives |
|---|---|---|
| **L_1a96c** (3114) → L_1a31c | time, rule, vague | skipping → `N_fa(surf0,0)` + state swap; else rule<0: `N_fb(surf0,"cross",1,1,time)` (default 1000); else `G[831]=N_e5("SYS\RULE%03d.ELG",…)` and `N_fb(surf0,"rule",1,1,time,G[831],vague)` (vague default 32); then state swap, G[829]=1. e.g. `(300,-1,0)` ×1156, `(2000,-1,0)`, `(500,40,500)` |
| **L_19d58** (7274) | 1, 0 | wait loop: yield while `N_fd(surf0)`; on skip input `N_fc`; G[829]=0 |
| **L_1ae2c / L_1abf0 / L_1ad18** → L_1a9e0 | …, time, p1(=86), keep | `[keep: N_fa(surf0,0)]`, `N_fb(surf0,"trans",1,!keep,time,p1,keep)` |
| **L_1aea0** (86) | time, a1, a2 | `N_fb(surf0,"scroll",1,1,time,a1,a2)` |
| **L_1b040** (241) | type 0..6, time, p2,p3,p4 | `N_fb(surf0, zoomflip/wave/mosaic/rotaxis/rotcenter/addsub/hblur, 1,1, time, …)` |
| **L_19f9c** (463) / **L_1a15c** (93) | time | back page := black / white colour, clear chars, cross-fade |
| **L_1b888** (524) | colour, time | blocking flash: crossfade to colour (4t) and back (2t), t=time/6 (time<6 → 600) via L_2a150 |
| **L_2a150** | skipMask, time, swapMode, swapChars, surf | blocking `N_fb(surf,"cross",swapMode,1,time)` |
| **L_1b69c** (939) | time, ampX, ampY, a3, "0"/"1", a5 | `N_fb(surf0,"quake",1+!a5,0,time,…)` (no state swap) |

### 6.4 Background / CG wrappers

| wrapper (calls) | args | behaviour |
|---|---|---|
| **L_a5d0** (1342) | bgNo, variant, page | numbered BG: stem table by number+variant (A/B/C/D suffixes; some with initial scroll: BG02/19/39/44/45/64/65 y=600, BG03/04/63 x=400, BG50 y=32; BG06/07 random of 8 via `N_52(8)`); bgNo≤0 → black. Then `L_1d070("PIC\%s.ELG",page)` (+ `L_ed64` offset). Returns 1 if changed |
| **L_9ef4** (263) | file, page | raw image as bg (CGs): `L_1d070(file,page)`, geometry reset |
| **L_9b88** (518) | colour, page | solid colour bg via L_1cad0 |
| L_1d070 | file\|0\|−1, page | `N_ef(old)` (unless shared), `G[898]{p}=N_e4(file,0)`, `N_19d(G[896]{p},img,0,0,1)`, `N_19e(…,0,2)`; 0/−1 → black/white |
| L_1cad0 | colour, page | drop image, `N_19e(G[896]{p},colour,1)` |
| **L_1a730** (538) / **L_1a858** (657) | bgNo,variant / file, time, rule, filter, vague | set bg on page 1 (+filter), clear chars, `L_1a31c` transition. e.g. `L_1a858("PIC\EF01.ELG",200,34,0,0)` |
| **L_ecd4** (298) → L_1d4bc | filter, page | destructive in-place image filters on G[898]{p} (clones first if shared): low 3 bits 1 gray (`N_175`), 2 sepia (`N_175`+`N_176(img,152,128,107,24,0,-4)`), 3 negative (`N_177`), 4 brightness (`N_15a(img,150)`); 8 letterbox (`N_13e` black bars 0..75, 525..600); 16 `N_187`, 32 `N_186`, high 16 bits n → `N_185(img,n,n,0)`; then `N_1a4` |
| L_ed64 | x, y, page | `N_19f(G[896]{p},x,y)` scroll |
| L_f584 | x,y,w,h,page | `N_1a0(surf,0,0,800,600,x,y,x+w,y+h)` zoom |
| L_f8c4 / L_fb94 | x,y,angle,zoom,page | `N_1a2(surf,x,y,angle,zoom)` |
| **L_eff0** (121) | on, page, flag | freeze: `N_1a8(surf,snap,2,-1,flag)`, `N_1a9(surf,1,flag)`, `N_19d(surf,snap,0,0,1)`; off restores |
| **L_10428** (126) / L_10150 (249) | motion, time, …, page | `N_d6(surf,"bg_move",motion,time,…)` / instant `N_d9("bg_move",…)` |
| L_10980 / L_fdf4 | cx,cy,scale,accel,time,page | `bg_zoom` animated / instant |
| L_1107c | x,y,angle,zoom,str,time,page | `bg_rot` |
| **L_12998** (63), L_12e94 | … | `bg_blur` |
| L_11ea0 (24) | page, type, a2..a8 | type 0 `bg_rast`, ≥1 `bg_haze` with preset G[881][type-1] ("(1,0,10)","(1,0,20)"). s01_01a: `L_11ea0(1,1,0,400,3000,0,1000,1000,0)` |
| L_11b0c (52) / L_12588 (48) | … | `N_d6(surf0,"noise",level,1993,…)` / weather `paper|light|snow|rain` |
| **L_1bd8c** (121) | time\|−1, … | `G[836]=N_d6(surf0,"quake",…)` (−1 endless) |
| L_11678 (177) / L_11784 (157) | (skippable,) page | wait for / kill bg effect task (`N_cf` poll, `N_d0`) |

(Note: `sp_move`, `bg_move`, `quake`, `noise`, weather names are accepted by `N_d6`/`N_d9`
although only the six `bg_*` names are registered in 0x411f50 – the others are registered
elsewhere, e.g. by plugins; see the effect doc.)

### 6.5 Character wrappers

| wrapper (calls) | args | behaviour |
|---|---|---|
| **L_1c43c** (11049) | slot, name, time, pos, rule, vague | main "show/change character": `L_83fc`; `L_14084(slot,name,1)`; pos≥0 → `L_17514(slot,pos,0,1)`; `L_1a31c(time or 300, rule, vague)`; scene then `L_19d58`. e.g. `(0,"KAZ01G",0,0,-1,0)` |
| **L_14084** (4249) | slot, name\|0, page | set/clear a standing picture; same name & no filter → no-op; else L_14f98 |
| L_14f98 | slot, name, bringFront, page | free old sprite; z shuffle; `img=N_e4("CHR\%s.ELG",0)`; `spr=L_1ef58(img,0xffff,0,0,0, G[777]-400+offX, G[778]+offY, 0,0, 2000+z, 1, 0, G[896]{p})`; `N_1c2(spr,1)`; auto tint (5 evening / 6 night) |
| L_1ef58 | img,l,t,r,b,x,y,useAlpha,alpha,prio,visible,overlay,surf | `spr=N_18d(img,l,t,r,b,x,y)`; (l==0xffff → `N_1b2(spr,x,y)`); useAlpha → `N_1b0(spr,2)`,`N_1b1(spr,alpha)`; prio≥0 → `N_194(surf,spr,prio,G[903],overlay)`; `N_1ac(spr,visible)` |
| **L_1c5b0** (644) | time, rule, vague | clear all chars with transition |
| **L_19a68** (1206) | page | clear all 6 slots |
| **L_17514** (4568) | slot, pos, offscreen, page | pos 0/1/2 = x 400/200/600 (offscreen: below / off left / off right); `N_1b2(spr, x-400+offX, y+offY)` |
| L_17e88 (112) / L_182a8 (424) | slot, apply, page | bring to front (`N_199(surf,spr,2000,0)`) / send to back (`N_19a(surf,spr,2005,0)`) |
| **L_18b74** (2363) | slot, motion, ms, page | `G[828][i]=N_d6(G[896]{p},"sp_move",spr,motion,ms)`; final x/y/z queried with `N_d8` and stored at once |
| L_186c8 (362) | slot, motion, page | instant final position via `N_d9("sp_move",…)`; alpha≥0 → `N_1b1` |
| **L_19808** (2256) | slot, 1, 0 | wait for the motion task |
| L_193a4 (30) | slot, ms, s1, s2, a4, page | `N_35` animator (second animator class) |
| **L_15a04** (324) | slot, flags, page | destructive colour filters on the char image (clone if shared): 1 gray, 2 sepia, 3 negative, 4 `N_15a(img,128)`, 5 evening `N_176(img,129,128,120,15,0,0,0xffff)`, 6 night `N_176(img,126,128,115,-7,-7,25,0xffff)`, 0x10/0x20/high bits plugin filters; then `N_1ae(spr)` |

Motion strings (sp_move/bg_move, parser 0x402970): space-separated sequential segments;
`c` home (logical 400,0), `l`/`r` home x ∓ 200, `u`/`d` vertical edges, `!l !r !c !u !d`
off-screen, `<n >n` x ∓ 25n, `-n +n` y ∓ 25n, `a±d.d` easing, `i`/`o` end alpha 255/0,
`f`/`b` front/back, `$ % [ ] ^ _` in-place gestures (guess). Details belong to the
effect doc.

### 6.6 Message window, load replay, frame loop

* `L_7480(show, yield)` (991): `N_2d1` show/hide of message window G[810], choice G[812],
  frame buttons G[817][0..9] (UI doc). `L_7720(show, ms, skippable)` fades them with
  `N_1b1`.
* Load: `L_22364` sets G[905]=1 and re-runs the scene silently; at the target line
  `L_2c3c` restores the 780-byte block and rebuilds both pages: per char
  `L_14f98`+filters+`N_199(G[896]{p}, spr, 2000+z, 0)`; per page bg (anime / file /
  colour, filters, freeze, zoom/rotation/scroll); persistent effect; BGM; then
  `N_1a8(G[896]{+0}, G[896]{+4}, 2, -1, 0)`, `N_13e(G[896]{+0},0,0xffff)` and a 500 ms
  cross-fade (`L_2a150(1,500,0,1,0)`). This is a complete spec of the saved visual state.
* There is no script-side per-frame layer loop: all animation is native tasks; script
  waits poll `N_fd`/`N_cf` once per frame (`end`), and the logical state variables are
  set to final values when an animation starts.

---------------------------------------------------------------------------------------

## 7. Reimplementation checklist

1. Image = BGRA32 (or 8-bit + palette) buffer with colour key, ELG offset, has-alpha, tag.
2. Page = image + compositor (bg image/offset/colour, main list, optional overlay list,
   bg transform cache) + link to a back page + transition slot.
3. Layer objects: Sprite (src image, src rect, pos, mode, opacity, visible, tag,
   optional zoom/rotation, scroll offset), ColorRect. Keep lists sorted ascending by
   priority; draw from highest priority value to lowest, then the overlay list.
4. Blend: mode 0 = copy (opacity 255) / constant alpha; mode 2 = source-alpha (× opacity/256
   via `(sa*opa)>>8`); use the packed `>>8` formulas for bit-exactness.
5. Scheduler with typed tasks, pause/resume/kill-by-type; path animator per §4.
6. Frame: tick tasks → compose dirty (or all) → views in descending priority → present.
   60 fps gate.

---------------------------------------------------------------------------------------

## 8. Open questions

* Exact semantics of tvpgl modes 1, 3–31 (only mapped to kernel addresses; names are
  guesses from TVPInitTVPGL order). Not needed unless a script/UI path sets them (none
  found for sprites).
* Interpolating stretch path (tvpgl:0x10004cb0) not decoded (bilinear assumed); bg zoom
  default filter value 1 means nearest.
* comp+0x51 meaning; S+0x15c/+0x19c full-screen
  rect semantics (only relevant to window scaling).
* Type-8 surface class (ctor 0x4cf4b0) and type-0 objects: creators not among used natives.
* DD-mode page rendering during transitions (0x4cee60/0x4ce2b0/0x4ce860) – covered by the
  transition doc; assumed to output the blended image to the page's DD surface.
* LAG/LGM layouts only sketched (owned by the UI/animation natives).
* Task type IDs 10003/14001/14003/13000/13002 mapped only by vtable location (guess).
* `N_1c2(spr,1)` (set by every character sprite) – flag meaning (filter for zoom/rotation) unverified.
* `N_d9("sp_move",0,2000,2006)` in gameinit (probably the character priority range).
