# Visual effects and transitions (ORE_TUBA.EXE 1.01)

Scope: screen transitions (`N_fa..N_fd` + the transition registry), layer/BG
effects (`N_d6..N_d9` + the layer-effect registry), the image-processing natives
`N_13b..N_18c`, the FUNC_LIB wrappers that scene scripts call, and the
`tvpgl.dll` routines behind all of this. The core sprite/layer/page system is
documented elsewhere; here it is only referenced ("stage", "page", "layer
object", "work surface").

Conventions
* Addresses are VAs in ORE_TUBA.EXE (base 0x400000) or `tvpgl.dll` (base
  0x10000000).
* Pixel format everywhere is 32-bit little-endian `0xAARRGGBB` (bytes B,G,R,A).
* "C ref" = the portable C routine inside tvpgl.dll. At run time the DLL may
  pick MMX/SSE variants (patched in at 0x1000820x..); they implement the same
  formula, though rounding may differ by ±1.
* `[verified]` = read directly from code; `[likely]` = strongly suggested but
  part of the math hidden by x87 code or not fully traced; `[guess]` = guess.

How this was produced: `build/re/ore_tuba.c` doesn't include vtable methods
(Ghidra didn't find them). They were decompiled from a scratch copy of the
Ghidra project with a small `DecompAt` script (create function at address,
then decompile). tvpgl.dll was imported into a scratch Ghidra project. Neither
build/ nor any project file was modified.

---------------------------------------------------------------------------

## 1. Architecture

### 1.1 Two name registries
Both are created in the app constructor (`FUN_00411600`-area, ore_tuba.c:11651)
and filled at 12049..12066 and in `FUN_0041d460` (ore_tuba.c:16450):

| registry | object | global | entry |
|---|---|---|---|
| **transitions** | app+0x19cc, vtable 0x4edd0c | `DAT_00503224` | 0x14 bytes `{u8 type; char* name; fn factory; ...}` (`FUN_00439af0`) |
| **layer effects** | app+0x19f0, vtable 0x4edce4 | `DAT_00503220` | 0x1c bytes `{u8 type; name; factory; helper(+0xc); query(+0x10); ...}` (`FUN_0047e7a0`) |

Lookup is by exact (case-sensitive) string compare. Transition creation
(`FUN_00439fd0`, registry vtbl+0x24): `obj = factory(); obj->vt[0x1c](targetPage, args)`.
Layer-effect creation (vtbl+0x20, `FUN_0047ec70`): `obj = factory()`; the native
then calls `obj->vt[0x34]`. `FUN_0047ed60(reg,name)` returns the entry's
"query" helper (+0x10), used by `N_d9`.

**Transitions** (`type` byte from 0x4ee928.. / 0x4eda8c..):

| name | factory | vtable | obj size | type | update | done | render | init |
|---|---|---|---|---|---|---|---|---|
| cross | 45d8b0 | 4ee930 | 0x14 | 0 | 40af40 | 40afa0 | 45d8d0 | 45d920 |
| rule | 45d970 | 4ee958 | 0x1c | 0 | 40af40 | 40afa0 | 45d990 | 45da20 |
| scroll | 45db20 | 4ee980 | 0x20 | 0 | 45e520 | 40a5c0 | 45db70 | 45deb0 |
| wave | 45dff0 | 4ee9a8 | 0x4c | 0 | 45e030 | 45e190 | 45e1a0 | 45e390 |
| mosaic | 45e4e0 | 4ee9d0 | 0x20 | 1 | 45e520 | 40a5c0 | 45e560 | 45e800 |
| rotaxis | 45e910 | 4ee9f8 | 0x18 | 1 | 45e950 | 45e9a0 | 45e9b0 | 45ed80 |
| trans | 45eea0 | 4eea28 | 0x18 | 1 | 45eee0 | 45ef50 | 45ef70 | 45f000 |
| tone | 45f060 | 4eea50 | 0x18 | 2 | 45f0a0 | 45f110 | 45f130 | 45f1f0 |
| quake | 409e60 | 4eda94 | 0x38 | 2 | 409ef0 | 40a280 | 40a290 | 40a4b0 |
| zoomflip | 40a530 | 4edae4 | 0x24 | 1 | 40a570 | 40a5c0 | 40a5d0 | 40a8e0 |
| rotcenter | 40aa70 | 4edb0c | 0x1c | 1 | 40aab0 | 40ab00 | 40ab10 | 40add0 |
| addsub | 40af00 | 4edb34 | 0x14 | 0 | 40af40 | 40afa0 | 40afb0 | 40b170 |
| hblur | 40b1e0 | 4edb5c | 0x20 | 1 | 45e520 | 40a5c0 | 40b240 | 40b500 |

Transition vtable: +0 dtor, +4 type(), +8 name(), **+0xc update(nowMs)**,
**+0x10 done()** (returns −1 when finished, else 0), **+0x14 render(ctx*)**,
+0x18 init-varargs → +0x1c, **+0x1c init(page, int* args)** (0 = ok),
+0x20/+0x24 return −2. All init functions store `args[0]` (duration ms, must be
nonzero) at +8 and set a "first update" flag, so the first `update()` stores
`start = now` and progress 0.

Type meaning (from the run loop 0x4bd580): type 0/1 need two sources (old and
new page); type 2 uses only the current image (src2 = null). Type bit0 = 0
means "row-local", so the run loop may render it in horizontal bands; type 1
is always rendered as one full frame. Both produce the same image.

**Layer effects** (type byte 0x4ee0d4.. / 0x4ef914 / 0x4ed490..):

| name | factory | vtable | type | run (+8) | init (+0x34) | ctrl (+0x3c, N_d7) | get (+0x44, N_d8) | N_d9 helper |
|---|---|---|---|---|---|---|---|---|
| bg_zoom | 43cda0 | 4ee0dc | 0 | 43cdd0 | 43d100 | – | 43d260 | 43d010 |
| bg_rot | 43d400 | 4ee140 | 0 | 43d430 | 43d520 | – | – | – |
| bg_rast | 43d740 | 4ee190 | 0 | 43d770 | 43d820 | 43d930 | 43d960 | – |
| bg_haze | 43dbb0→43dc10 | 4ee1e0 | 0 | 43dcc0 | 43df60 | 43e080 | 43d960 | – |
| bg_blur | 43e3d0→43e430 | 4ee230 | 0 | 43e5c0 | 43eae0 | 43edf0 | 43ee40 | 43f400 |
| bg_anime | 488ba0→488c00 | 4ef918 | 0 | 4891f0 | 489390 | 489530 | 489700 | 489810 |
| sp_move | 402f50 | 4ed498 | 1 | 403190 | 403460 | – | 403550 | 4033a0 |
| bg_move | 4037d0 | 4ed570 | 0 | 403900 | 403b50 | – | 403d80 | 403ac0 |
| quake | 404180 | 4ed5c0 | 3 | 404240 | 4046b0 | – | – | – |
| noise | 404a80 | 4ed628 | 3 | 404c20 | 404dc0 | 405000 | – | – |
| paper | 405060→405890 | 4ed6c0 | 3 | 405a40 | 405af0 | 405e10 | – | – |
| light | 405e80→4067a0 | 4ed738 | 3 | 406950 | 406a00 | 406d30 | – | – |
| snow | 406da0→407460 | 4ed7d0 | 3 | 407610 | 4076c0 | 4079e0 | – | – |
| rain | 407a50→408070 | 4ed830 | 3 | 4081c0 | 408270 | 408540 | – | – |

Layer effects are tasks of the scheduler `DAT_00503214` (vtable +4 =
`add(task, ...)`, +8 = remove). Task vtable: +4 priority (14000 for BG
effects, 14001 for sp_move), **+8 run(nowMs)** returns nonzero ⇒ task finished
and removed, +0xc "stop/restore" (called on removal), +0x1c pause(now)/+0x20
resume(now) (shift start time by the pause length, `FUN_00402f90/402fb0`),
+0x34 init(scheduler, layerData, int* args), +0x4c "auto-stop/finish"
(called by init when the last arg flag is set).

"layerData" is the BG/char layer object (`page->vt[0xc]()`): +0x44 base image
surface, +0x58 work surface (what the layer displays while an effect writes
into it), +0x48/+0x4c image scroll offset, +0x50 background colour,
`FUN_0043aa90(layer,0)` allocates the work surface, `FUN_0043b390(layer)` marks
the layer dirty, `FUN_0043ab90(layer,0,0,W,H,rect)` sets the visible source
rect (zoom view), `FUN_0043af80(layer,cx,cy,angle,zoom)` sets rotation/zoom.

### 1.2 Time base
All effects use absolute milliseconds from `GetTickCount()` or `timeGetTime()`
(`DAT_00503869` picks). Each object computes `elapsed = now - start` with
unsigned 32-bit arithmetic. The run loop calls `update(now)` then `render()`
once per displayed frame. There is no fixed tick.

### 1.3 Shared tables and helpers
* **Integer cosine table**: `DAT_0050387c` → object built by `FUN_00466340`
  (app+0xef8). `icos[i] = (int)trunc(cos(i·π/256)·65536)` for i=0..511
  (icos[0]=0x10000, icos[256]=−0x10000, icos[128]=icos[384]=0), at
  `DAT_0050387c+4+i*4`. Code uses `icos[(k-128)&511]` as **sin(2πk/512)**.
  The same object holds `atan16[256]` at +0x804 =
  `trunc(atan(i/256)·32768/π)` (unused here).
* **Double cosine table** (lazily, `FUN_00466480`): `dcos[i]=cos(i·π/256)`, 512
  doubles at `*(DAT_0050387c+0xa04)`. Angle unit everywhere = **512 per turn**.
* **Blend weight table** `0x5040c0` (8 bytes per entry, built in
  `FUN_004c03b0`): entry[0] = 0, entry[a] = (a+1, a+1, a+1, 0) as 4×u16 for
  a=1..255. ORE's own lerps use `w = a ? a+1 : 0` and compute
  `d + ((s-d)*w >> 8)` per channel (packed 0x00ff00ff/0x0000ff00 arithmetic).
  Alpha is kept from the destination.
* **Clamp table** `0x524bc0`: `clamp[x]` for x in −768..1023 → 0..255.
* **Multiply table** `0x524fc0`: `mul[b][a] = (a*b + 127) / 255`
  (256×256 bytes; row b at 0x524fc0+b*256). `0x534ec0 = mul[255]`.
* **RNG** (`DAT_00503638`, used via `DAT_005031b0/190/198/19c/1a4/1a8`):
  Knuth's subtractive generator (Numerical Recipes `ran3`). 55 ints in
  [0,1e9), u16 index at 0x503638, refill loops `FUN_00404650`. Seeded once at
  start. Exact sequences don't need to match; any uniform RNG works.
* **Easing** (bg_zoom, bg_rot, bg_blur, move language): exponent `e` (double,
  parsed with atof from a string):
  `e<0: p' = 1 - (1-p)^(-e)` (ease-out); `e>0: p' = p^e` (ease-in);
  `e==0: p' = p`. Implemented with `_CIpow` (0x4dba40) on `t/D`; the
  result is turned back into ms (`trunc(D·p')`) [verified 0x43ce07..].
* `__ftol` (0x4dba14) **truncates toward zero** (sets RC=11b).
* `FUN_0045d820(blit, dst, dx, dy, src, srcRect)`: clipped rectangle copy
  (blit = `FUN_0045f780` C / `FUN_0045f940` MMX; straight 32-bit copy
  including alpha).

---------------------------------------------------------------------------

## 2. Transitions

### 2.1 Natives
All take a stage/page object as arg0 and unwrap it: type 7/9/10 → sub-object
at +0x30, type 8 → +0x54 (`S` below). Methods of S (vtable 0x4eee50 family):
+0x14 back page, +0x3c `swapPages(back)` (0x472210: exchange the layer trees of
front and back), +0x40 `copyPage(back)` (0x4722d0: clone back tree into front),
+0x4c `isTransitioning` (`S+0x98 != 0`), +0x58 start (0x4d1420), +0x5c finish
(0x4d15e0).

| native | addr | args | behaviour |
|---|---|---|---|
| N_fa | 426ec0 | (stage, m) | instant page action: m=0 swap, m=1 copy back→front, m=2 swap then copy. Returns 1. |
| N_fb | 426f50 | (stage, name, mode, flag, a0, a1, …) | create transition `name` with args a0.. (a0 = duration ms); `S->start(tr, (char)mode, flag!=0, !skipMode)`; returns S->start's result (0 ok) or −2 if the name is unknown. |
| N_fc | 427010 | (stage) | finish now: `S->finish(S->vt[0x24]())` (flag = stored "do page action") |
| N_fd | 427090 | (stage) | is a transition running (0/1) |

`start` (0x4d1420): finishes a running transition first. Stores `S+0x98=tr`,
`+0x9c=mode` (0 = no page action, 1 = **swap pages at end**, 2 = **copy back to
front at end**), `+0x9d=flag` ("perform the page action when finished"),
`+0x9e`. Allocates the frame buffer `S+0x94` (screen-sized surface,
`FUN_0043f750/0043f830`). While a transition runs, the stage displays
`S+0x94` (`FUN_004d1380`). `finish` (0x4d15e0) deletes tr. If flag is set it
swaps (mode 1) or copies (mode 2), then invalidates the whole screen. The
optional hooks `DAT_0050386c/00503870` (LAB_00414000/00414090) only adjust
thread priority.

### 2.2 Run loop (0x4bd580, method of the stage object; S = this+0x30)
Per frame:
1. `if (tr->update(now) != 0) → finish` (no current transition returns nonzero).
2. If the live flag `this+0xbc` is set, re-render the front and back layer trees
   (`FUN_0043b800`) so animated layers keep moving during the transition.
   Otherwise the page images are taken as they are.
3. Build the **render context** (13 ints):
   ```
   ctx[0..3]  = x, y, w, h        rectangle being produced
   ctx[4]     = dst surface       (the transition frame buffer S+0x94)
   ctx[5..6]  = dst x, y
   ctx[7]     = src1 surface      = OLD image (the stage's current front image)
   ctx[8..9]  = src1 x, y
   ctx[10]    = src2 surface      = NEW image (back page composite), null for type 2
   ctx[11..12]= src2 x, y
   ```
   All origins equal the rectangle (full screen 800×600 at 0,0). For band
   rendering (type bit0==0 and config `DAT_004fc479`), the frame is cut into
   horizontal strips of 8/16/32/64/128 rows (by width) and render() is called
   once per strip. The result is the same.
4. `tr->render(ctx)` writes the frame into the buffer, which is then blitted to
   the window.
5. `if (tr->done()) → S->finish(S+0x9d)`. The last frame (progress = 100%) is
   rendered before the finish.

Progress convention: **progress 0 shows src1 (old), 100% shows src2 (new).**

### 2.3 Per-transition algorithms
`t = now - start` (ms), `D` = duration (args[0]).

**cross** (args: D). `a = min(255, t*255/D)` (u8, integer division, update
0x40af40). Render calls DLL `cross` (§4.2) with opa=a, mode 0: a==0 copies
src1, a==255 copies src2, else per pixel
`d = s1 + ((s2 - s1)*a >> 8)` on B,R and G (C ref `TVPConstAlphaBlend_SD`,
0x100169c0). Output alpha = 0 (display ignores alpha). Done when a==255.

**rule** (args: D, ruleImage(8-bit surface), vague). Progress byte `a` as in
cross. Render calls DLL `rule` (§4.2) with `opa=a`, vague, mode 0. The rule
pixel is read at the same screen coordinate (ctx[0],ctx[1]). Math (C ref,
Kirikiri "universal transition"):
```
phase = ((vague + 256) * a) >> 8
table[i] (i=0..255) = 255                        if i <  phase-vague
                    = clamp(255 + (phase-vague-i)*255/vague, 0, 255)   if i < phase   (C int division)
                    = 0                          if i >= phase
w = table[rule(x,y)];  d = s1 + ((s2 - s1)*w >> 8)   (B,R,G; alpha 0)
```
If `vague < 512` the "switch" routine is used: `rule >= phase` → copy s1,
`rule < phase-vague` → copy s2, else blend as above (same result). a==0 →
copy s1, a==255 → copy s2. Dark rule pixels change first. init (0x45da20)
throws unless the rule surface has bpp 8 (surface+0x40 == 8).

**scroll** (args: D, dir, mode). `max` = page width (dir 0/2) or height
(dir 1/3); `p = min(max, t*max/D)`. Done when p==max. Positions (x for dir
0/2, y for 1/3; W,H = page size):

| dir | mode 0 (new slides in over old) | mode 1 (old slides out over new) | mode 2 (push) |
|---|---|---|---|
| 0 (→) | old 0; new at p−W | old at p (drawn on top), new 0 | old p, new p−W |
| 1 (↓) | old 0; new at p−H | old at p, new 0 | old p, new p−H |
| 2 (←) | old 0; new at W−p | old at −p, new 0 | old −p, new W−p |
| 3 (↑) | old 0; new at H−p | old at −p, new 0 | old −p, new H−p |

Plain copies. The image drawn underneath is clipped against the one on top.

**wave** (args: D(≥2), mode, A, P, color0, color1; `half=D/2`, `H` = page height).
update (0x45e030):
```
tc = min(t, D)                         (+0x40)
u  = tc < half ? tc : (tc < D ? D - tc : 0)      // triangle 0→half→0
s  = icos[((u<<7)/half - 128) & 511]   // = 65536*sin(π/2 * u/half): 0→1→0
amp  = (A*s) >> 16                     (+0x34)
step = mode0: (P*s)>>16 | mode1: (D-tc)*P/D | mode2: P*tc/D | else: P   (+0x38)
base = mode<3 ? -(H/2)*step : (P*tc/D)*A                              (+0x3c)
a    = min(255, tc*255/D)              (+0x44, done when 255)
fill = lerp(color0, color1, w[a]) on RGB, alpha of color0             (+0x48)
```
render (0x45e1a0), per output row y (absolute row index ctx[1]+row):
```
ph  = y*step + base   (u32)
off = (icos[((ph >> 9) - 128) & 511] * amp) >> 16     // amp*sin(2π·(ph>>9)/512)
if off > 0: fill dst x in [max(0,x0), min(off, x0+w)) with `fill`
if off < 0: fill dst x in [max(W+off,x0), min(W, x0+w)) with `fill`
for x in [max(off,x0), min(off+W, x0+w)):
    dst[x] = ConstAlphaBlend_SD(src1[x-off], src2[x-off], a)   // DLL table 0x174
```
Both images shift horizontally by the same row offset while cross-fading.
Script usage: (D,3,550,768,0,0), (D,0,96,0,0,0), (300,0,256,2048,..).

**mosaic** (args: D, origin 0..4 (>4 → 0), maxBlock (≥1), keepAspect).
`M = 2*maxBlock`, `p = min(M, t*M/D)`, done when p==M. If p < maxBlock: src = src1,
`b = p`; else src = src2, `b = M - p`. If b < 2: plain copy. Block size
`bw=bh=b`. With keepAspect, the smaller dimension is scaled:
`w<h: bw = max(2, w*b/h)`, else `bh = max(2, h*b/w)`. The first (partial)
block size per axis (0 = full block):
origin 0 center `((w-bw)/2) % bw`, 1 top-left 0, 2 right `w % bw`,
3 bottom `h % bh` (x full), 4 bottom-right both. Each block is filled with the
source pixel at the block's top-left corner (point sampling).

**rotaxis** (args: D, mode 0..3). `u = min(256, t*256/D)`, done at 256.
`k = u ≤ 128 ? u : u + 256` (angle index, 512 per turn), `c=dcos[k&511]`,
`s=dcos[(k-128)&511]`. Unit quad corners TL(−1,1) TR(1,1) BL(−1,−1) BR(1,−1)
are rotated:
mode0 `(x·c, y, −x·s)`, mode1 `(x·c, y, x·s)` (about the vertical axis);
mode2 `(x, y·c, y·s)`, mode3 `(x, y·c, −y·s)` (about the horizontal axis).
Perspective projection, camera distance 2:
`sx = trunc((2x/(z+2) + 1) * (W/2))`, `sy = trunc((1 - 2y/(z+2)) * (H/2))`.
If u ∉ {0, 512}, the area around the quad is cleared to 0 (four `fill`
rectangles, DLL +0x08). The source (src1 if u ≤ 128 else src2) rectangle
(TL,TR,BL,BR = (0,0),(w−1,0),(0,h−1),(w−1,h−1)) is then mapped onto the
projected quad by `FUN_00445a30 → FUN_004600f0`: a scanline polygon filler
with texture coordinates interpolated linearly along edges and spans (no
perspective correction), nearest sampling [likely].

**trans** (args: D, patternId, reverse). `u = min(256, t*256/D)`; when reverse,
`u = 256 - u` (first frame 256). Done at 256 (or 0 if reversed). Render:
copy src1 → dst, then `FUN_004a14e0(id, dst, src2, u, x, y, 0, 0)`, which
dispatches into an **87-entry pattern table at 0x4ffac4**. The scripts only use
**id 86 (0x4b1000)**:
```
if u > 255: copy src2 fully
H=src height, half=H/2, d = (half*u) >> 8
copy src2 rows [half-d, half)  → dst rows [0, d)          (top half slides down)
copy src2 rows [half, half+d)  → dst rows [H-d, H)        (bottom half slides up)
```
A "shutter close": the new image's halves come in from the top and bottom
edges and meet in the middle. FUNC_LIB uses reverse=1 together with `N_fa(stage,0)`
(swap pages first) and `flag=0`. Then src1 = new and src2 = old, and the
shutter opens to reveal the new image.

**tone** (args: D, color, reverse; unused by the scripts). `a=min(255,t*255/D)`,
inverted if reverse. a==255 → fill dst with color (DLL +0x08). Otherwise copy
src1 and, if a≠0, DLL +0x10 `(dst, color, a, rect)` = constant-color alpha
blend.

**quake** (transition; args: D (−1 = endless), ampX, ampY, clearBorder,
decayStr). update (0x409ef0, x87 code read from disassembly):
```
if D != -1 and t >= D: state=done, X=Y=0
f = (D == -1) ? 1.0 : 1 - (1 - decay) * t / D        // decay = atof(decayStr)
r() = rand/1e9 in [0,1)
if ampX == ampY: X = trunc((r()*ax - ax/2)*f); Y = trunc((r()*ay - ay/2)*f)
if ampX <  ampY: X = trunc((r()*ax - ax/2)*f); Y = trunc(±r()*ay*f)   // sign alternates each update (toggle +0x35)
if ampX >  ampY: X = trunc(±r()*ax*f);       Y = trunc((r()*ay - ay/2)*f)
```
render: copy src1 shifted by (+X,+Y) (source rect = rect−(X,Y), clipped). If
clearBorder, the uncovered strips are filled with 0. Otherwise the previous
buffer content stays there. Type 2: works on the current image only.

**zoomflip** (args: D, mode, smooth). `max` = W (mode 0/2), H (1/3), 512
(4/5), 256 (6). `p = min(max, t*max/D)`. Uses DLL stretch (table +0x28, §4.3)
`stretch(dst, src, size{w,h}, dx, dy, srcRect|null, 0, smooth, opa, 0)`:
* 0: old squeezed into (p,0,W−p,H), new into (0,0,p,H) (new grows from left)
* 1: old into (0,p,W,H−p), new (0,0,W,p)
* 2: old (0,0,W−p,H), new (W−p,0,p,H)
* 3: old (0,0,W,H−p), new (0,H−p,W,p)
* 4/5: p ≤ 256: z=max(1,256−p), draw old from the centered crop
  (z·W/256 × z·H/256) to full screen (zoom-in); p>256: z=p−256, draw new
  from its centered crop (zoom-out). Mode 5 redraws only when p changes and,
  after the first frame and while z≠256, with opacity 8 (motion-trail effect).
* 6: copy old, then draw new from a centered crop of size (p·W>>8, p·H>>8)
  stretched to full screen with opacity p (opaque when p ≥ 255).
`smooth` = stretch filter (0 nearest, 1 linear) [likely].

**rotcenter** (args: D, mode, smooth). `u=min(512, t*512/D)`, done at 512.
mode 0: copy src1, then draw src2 scaled by u/512 (size `W*u>>9 × H*u>>9`,
centered) and rotated by angle u (512 = 360°) around its center. mode 1:
`u'=512-u`, copy src2, draw src1 (shrinks and spins away). mode 2: like 0, but
only when u changed, no background copy, and opacity 16 after the first
frame (trail). Rotation: corners TL,TR,BL of the scaled rect are rotated
about the rect center with `x' = c·dx − s·dy + cx`, `y' = s·dx + c·dy + cy`
(c = dcos[u], s = dcos[u−128]). They are passed to the DLL affine blit
(table +0x2c, `FUN_10007800`). Nothing is drawn if the size is ≤ 1.

**addsub** (args: D, mode). `a` = cross progress byte. a ≥ 255 → copy src2.
Otherwise: base = src1 with k=a (mode bit0 = 0), or src2 with k=255−a (bit0 = 1).
Copy the base, then per pixel RGB (alpha kept): mode < 2 → **saturated add**
(k,k,k) (`FUN_00466790`), mode ≥ 2 → **saturated subtract** (`FUN_004669c0`).
Mode 0: old washes out to white, then cut; 1: new appears from white; 2: old
to black; 3: new from black.

**hblur** (args: D, mode, R (clamped 1..127)).
mode 0: `M=2R+1`, `p=min(M,t*M/D)`; p ≤ R: radius p on src1; else
radius M−p on src2; radius 0 → plain copy.
`FUN_0046a420`: horizontal box blur, width 2r+1, all 4 channels, edges clamped
(edge pixel repeated), `out = (sum*k + 0x8000) >> 16`, `k = 65536/(2r+1)`.
mode 1: `M=255`, a=p; `r = R*a/127` (a<128) or `R*(255−a)/127`. r==0 → cross
fade with a; else copy src1 and blend blur(src2, r) over it with
`d + ((b−d)*w[a] >> 8)` (`FUN_0046a670`).

### 2.4 Rule images
`SYS\RULE%03d.ELG`, rule000..rule110 in SYS.LPK: all **800×600, 8 bpp, gray
palette** (index = value; 0 black … 255 white). The scripts use 27, 28, 39–42,
47, 48, 54, 65, 66, … (see §3). `SYS.LPK` also has `*.gmp` (button/hit maps,
"LGM" header with ELG names; not transitions) and `logo.lag` (a "LAG"
animation played by N_1f8/N_1fc, not part of this area).

---------------------------------------------------------------------------

## 3. FUNC_LIB transition/effect wrappers

Notation: `f(a,b,c)` = arguments in push order (the first pushed is the
first argument). Page 0 = front, 1 = back. `G[896][page]` = page objects,
`G[896][0]` = the stage used for transitions.

Bookkeeping helpers:
* `L+19ba8()`: if `G[829]` (an asynchronous transition is pending) and
  `N_fd(stage)` → `N_fc(stage)` (force-finish). Called at the start of every
  BG/char change, so **transitions are asynchronous**: the script goes on (text
  appears) and the next visual change completes the transition.
* `L+19b40()`: `G[829]=1`; `L+28018(1)` swaps all script-side page-0/page-1
  arrays (BG names, effect handles G[820..828], …) to match the page swap that
  will happen at the end; if `G[830]==1` → `L+2928c()`.
* `L+19c84()` (skip mode): `N_fa(stage, G[830]==2)` + same bookkeeping.
* `L+1c9b0(mask)`: true if the user is skipping (bit0: key state via
  N_227/N_242; bit1: N_234 / config N_9(0,0)). Skipping turns every transition
  into an instant page action.
* `L+9ef4(name, page)`: set BG image of page (name table G[757], 24 bytes per
  page). Stops the page's BG effect (`L+98e0` → `N_d0(&G[820][page])`).
* `L+9b88(color, page)`: page BG = solid colour.
* `L+a5d0(bgNo, variant, page)`: BG by number ("BG01","BG02A",…).
* `L+19a68(page)`: remove the 6 character slots of the page.

Transition entry points (all start with `19ba8`, set `G[830]=1`, use
`L+1c9b0(2)` for skip → `19c84`, otherwise `N_fb(...)` then `19b40`):

| entry | args | does |
|---|---|---|
| **L+1a31c** | (D, ruleNo, vague) | D=0→1000, vague=0→32. ruleNo<0: `N_fb(stage,"cross",1,1,D)`. Else load `SYS\RULE%03d.ELG` (N_e5 into G[831]); `G[789]` → N_187 (h-flip), `G[790]` → N_186 (v-flip); `N_fb(stage,"rule",1,1,D,G[831],vague)` |
| **L+1a96c** | (D, ruleNo, vague) | just `1a31c` (3116 calls) |
| L+1a858 | (file, D, ruleNo, ?, vague) | `9ef4(file,1)`; arg4≠0 → `1d4bc(arg4,1)`; `19a68(1)`; `1a31c` |
| L+1a730 | (bgNo, var, D, ruleNo, x, vague) | `a5d0(bgNo,var,1)`; …; `1a31c` |
| L+19f9c / L+1a15c | (D) | back page = black (0) / white (0xffffff), clear chars, `cross` D (default 1000). Fade to black/white |
| L+1a9e0 | (D, id, rev) | D=0→1000; rev → `N_fa(stage,0)` first; `N_fb(stage,"trans",1,!rev,D,id,rev)` |
| L+1ad18 / 1abf0 / 1ae2c | … | set BG (by file / number / none) then `1a9e0`. Always id 86 (shutter) |
| L+1aea0 | (D, dir, mode) | `N_fb(stage,"scroll",1,1,D,dir,mode)` |
| L+1b040 | (kind 0..6, D, a, b, c) | D=0→1000; kind: 0 `zoomflip(D,a,b)`, 1 `wave(D,a,b,c,0,0)`, 2 `mosaic(D,a,b,c)`, 3 `rotaxis(D,a)`, 4 `rotcenter(D,a,b)`, 5 `addsub(D,a)`, 6 `hblur(D,a,b)` |
| L+1b69c | (D, ax, ay, clear, decayStr, sync) | skip → nothing; D=0→500; `N_fb(stage,"quake",1+!sync,0,D,ax,ay,clear,decayStr)`; `G[829]=1`. Typical: (0,30,30,0,"0",1) |
| L+1b888 | (color, D) | **flash**: back page = color; D<6→600; d=D/6; synchronous `L+2a150` cross 4d (swap), then cross 2d back (swap again). Colors: 0xffffff, 0x6c272d, 0x001e43 |
| L+2a150 | (skipMask, D, mode, ?, stage) | **synchronous** cross: `N_fb(stage,"cross",mode,1,D)`, `yield` until `!N_fd`, or the skip check → `N_fc`. 2a4b4 / 2a840 = same for rule / trans |

Script usage stats (call counts): `1a96c(300,-1,0)` ×1157, `(500,-1,0)` ×464,
`(500,40,500)` ×372, `(500,65,500)` ×164, `(100,-1,0)`, `(500,47,500)`,
`(500,41,500)`, `(300,48,500)`, …; `1ad18(file,300,86,0,0)` ×555;
`1ae2c(500,86,1)` ×133; `19f9c(1200)` ×316.

### 3.1 Worked example: s01_01a
```
L+9ef4("PIC\EF05A.ELG", 1)            back page BG := EF05A
L+11ea0(1, 1, 0, 400, 3000, 0, 1000, 1000, 0)
      → page 1: N_d6(page1,"bg_haze","(1,0,10)",400,3000,0,1000,1000,0,0)
        heat-haze, table 400 rows, period 3 s, endless, 1 s fade-in/out
        (G[781]=4 and params saved because duration==0)
L+1a96c(2000, -1, 0)                  → N_fb(stage,"cross",1,1,2000)
        2 s crossfade old→new (the new page's haze keeps animating if the stage
        is in live mode), then pages are swapped (mode 1).
```

### 3.2 Effect wrappers (layer effects)
* **L+11ea0** `(page, kind, amp, n, period, dur, fadeIn, fadeOut, fill)`:
  stops the page's effect (+ layer quake `G[836]`). If effects are enabled (`N_9(0,6)`):
  kind 0 → `N_d6(page,"bg_rast",amp,n,period,dur,fadeIn,fadeOut,fill,dur!=0)`;
  kind k≥1 → `N_d6(page,"bg_haze",G[881][k-1],n,period,dur,fadeIn,fadeOut,fill,dur!=0)`
  with `G[881] = {"(1,0,10)","(1,0,20)"}` (USER_VAR initial strings, order [likely]).
  If dur==0 the state is saved in `G[781]=3+kind`, `G[782][0..4]=amp,n,period,fadeOut,fill`.
  Used: kind 1 always, n 300/400, period 2000/3000.
* **L+12588** `(kind, count, prewarm, f1, f2, page)`: weather on stage:
  `N_d6(stage, G[884][kind], count, 1993, 100, prewarm, f1, f2)`,
  `G[884] = {"paper","light","snow","rain"}` [likely order]. Saved as `G[781]=6+kind`.
* **L+12998** / **L+12e94**: `bg_blur` (numeric / key-frame string form, §5.5).
* **L+13300**: `N_d9("bg_blur", …)` final-state query.
* **L+10428** `(moveStr, stepMs, …)` / L+10150: `bg_move`; **L+10980**
  `(cx, cy, zoomStr, easeStr, D, page)`: `bg_zoom`; **L+1107c**: `bg_rot`;
  L+11da0 area: `N_d6(stage,"noise",opacity,1993,f1,f2)`;
  **L+1bd8c** `(D, ?, ax, ay, decayStr, snap, fBg, fFg, auto)`: layer `quake`
  on the stage (D=0→500; D=−1 endless, saved as `G[781]=2`).
* Character filters (FUNC_LIB 0x169dc, uses the image natives of §6) on char
  sprite images, by `mode&7`: 1 gray (N_175); 2 gray + sepia
  `N_176(152,128,107,24,0,-4)`; 3 invert RGB (N_177 0); 4 darken
  `N_15a(128)`; 5 `N_176(129,128,120,15,0,0)` (evening); 6
  `N_176(126,128,115,-7,-7,25)` (night); plus blur `N_185(r,r,0)` and flips
  N_187/N_186.

---------------------------------------------------------------------------

## 4. tvpgl.dll

### 4.1 Loading
`FUN_004654e0` loads `<gamedir>tvpgl.dll`, calls
`tvpglConfig(0, cb1, cb2, cb3)` (ORE row copy routines for blit methods 1..3:
`FUN_0045f780/0045f940`, `FUN_004b58a0/4b5aa0` (copy RGB keep dst alpha),
`FUN_004b5e10/4b5f00`), then `GetLibrary(&cpuflags)` (0x10001c10). That
returns a table whose entries are copied into ORE globals
(ore_tuba.c:63300..). `DAT_00503874` ≠ 0 means "DLL present". Most image ops
then go through the DLL; the transitions always do.

### 4.2 Lucifen wrappers (table +0x08..+0x68, `GetLibrary` → 0x10001000)
| off | ORE global | DLL fn | use |
|---|---|---|---|
| 08 | 0050394c | 10001d20 | fill ARGB (surf, color, rect) (`TVPFillARGB`) |
| 0c | 00503c6c | 10001e70 | fill 8-bit |
| 10 | 00503a3c | 10001fc0 | (surf, color, opa, rect): opa 255 → fill RGB keep alpha, else const-colour alpha blend |
| 14 | 00503ab0 | 10002130 | fill with colour/opacity, HDA variants |
| 24 | 00503a9c | 10002a50 | generic blit (dst, src, dx, dy, rect, method, opa, hda); method 2 = alpha blend, 3 = blend with opacity |
| 28 | 005039ac | 10005af0 | stretch blit (dst, src, size, dx, dy, srcRect, method, stretchType\|flags, opa, hda): 16.16 DDA nearest or linear-interpolated (`TVPInterpStretch*`) |
| 2c | 00503bd0 | 10007800 | 3-point affine (LinTrans) blit, smooth flag, opacity |
| 30 | 00503a78 | 1000ac60 | affine blit with a double[6] matrix |
| 34 | 00503c30 | 1000bbb0 | box blur (surf, rx, ry, rect, alphaFlag): Kirikiri `doBoxBlur`, 16-bit sums if (2rx+1)(2ry+1) < 256 else 32-bit |
| 38/3c | 00503c5c / 0050399c | 1000bde0 / 1000bf30 | vertical flip 32 / 8 bpp (swap rows) |
| 40/44 | 00503b54 / 00503b60 | 1000c080 / 1000c170 | horizontal flip 32 / 8 bpp (reverse each row) |
| 64 | 00503cb4 | 1000e8d0 | **cross** (dst,s1,s2,opa,mode,dx,dy,w,h,s1x,s1y,s2x,s2y): opa 0 → copy s1, 255 → copy s2, else per row `ConstAlphaBlend_SD` (mode 0), `_SD_d` (1), `_SD_a` (2) |
| 68 | 00503c24 | 1000ecd0 | **rule** (dst,s1,s2,rule,opa,vague,mode,dx,dy,w,h,s1x,s1y,s2x,s2y,rx,ry): see §2.3; mode 0/1/2 → plain/_d/_a variants |

Entries +0x6c onwards are the raw Kirikiri TVPGL function pointers, in
Kirikiri's `TVPGL_C_Init` order (assignment list at tvpgl 0x100082xx).
Index 66 (table +0x174, ORE `DAT_0050398c`) = `TVPConstAlphaBlend_SD`, used
directly by wave. 67/68 = `_SD_a/_SD_d`, 69..71 = `TVPInitUnivTransBlendTable`
(/_d/_a), 72..77 = `TVPUnivTransBlend` (/switch, _d, switch_d, _a, switch_a).

### 4.3 Formulas of the routines used (C ref)
* `ConstAlphaBlend_SD(d,s1,s2,len,opa)` (0x100169c0):
  `d = (s1&0xff00ff) + (((s2&0xff00ff)-(s1&0xff00ff))*opa >> 8) & 0xff00ff |
       (s1&0xff00)   + (((s2&0xff00)-(s1&0xff00))*opa >> 8) & 0xff00` (alpha 0).
* `InitUnivTransBlendTable(tab,phase,vague)` (0x10017070): see §2.3.
  The `_d/_a` table init is the same function (0x100170e0).
* `UnivTransBlend(d,s1,s2,rule,tab,len)` (0x10017100): `w=tab[rule]`, then as
  ConstAlphaBlend_SD with w.
* `UnivTransBlend_switch(...,src1lv=phase,src2lv=phase-vague)` (0x10017370):
  `r>=phase → s1; r<phase-vague → s2; else blend`.
* Box blur / flips: plain Kirikiri semantics, see the table above.

---------------------------------------------------------------------------

## 5. Layer effects (N_d6..N_d9)

### 5.1 Natives
| native | addr | args | behaviour |
|---|---|---|---|
| N_d6 | 426600 | (layerObj, name, a0..) | unwrap like N_fb, `e = registry->create(name)`; in skip mode (app+0xe37) clears byte e+0x24; `e->init(scheduler, layer, &a0)`; returns the effect handle or 0 |
| N_d7 | 4266c0 | (e, …) | `e->vt[0x3c](&args)` control (usually "stop/fade out") |
| N_d8 | 4266e0 | (e, …) | `e->vt[0x44](&args)` query (state / current values) |
| N_d9 | 426700 | (name, …) | static helper of the class: apply/compute the final state without animating (used for skip and save-restore) |
| N_d0 | 426540 | (&handle) | `*handle=0`; scheduler remove (calls the effect's stop/restore) |
| N_d1..N_d5 | 426560.. | (h) | scheduler pause/resume/query helpers |

### 5.2 bg_rast (raster wave)
init (0x43d820) args: `amp, λ, period, dur, fadeIn, fadeOut, fill, auto`
(+0x2c..+0x44). Needs the layer base image (+0x44). Allocates the work surface
and, if `fill==0`, copies the base image into it once.
run (0x43d770): `t = now - start`. State 0 fade-in: `A = amp*t/fadeIn` → state 1
(A=amp) at t ≥ fadeIn; state 1: if dur≠0 and `t-fadeIn ≥ dur` → state 2;
state 2→3: remember t₀, `A = amp - (t-t₀)*amp/fadeOut`; at the end restore
and return 1. `phase = ((t % period) << 9) / period`. Render (0x43d970) only
if (phase, A) changed; per row y, starting from `ph = phase<<16`, adding
`0x2000000/λ` per row:
```
dx = (icos[((ph>>16) - 128) & 511] * A) >> 16       // A*sin(2π(phase/512 + y/λ))
row y of work = row y of base shifted by dx (right if dx>0);
uncovered pixels = layer bg colour (+0x50) if fill, else left as they were
```
ctrl 0x43d930 = start the fade-out (if still fading in, the current A becomes
the start). get 0x43d960 = state byte.

### 5.3 bg_haze (heat haze)
init (0x43df60) args: `str, n, period, dur, fadeIn, fadeOut, fill, auto`.
`FUN_0043dd80(str, n)` builds an int table T[n] from `(f,φ,a)(f,φ,a)...`
groups (count = number of '(', SJIS-aware):
`T[i] = (Σ_groups icos[((i*512/n)*f + φ) & 511] * a) / groups >> 16`, so
"(1,0,10)" → `T[i] = 10·cos(2πi/n)`. The run state machine is the same as
bg_rast but the amplitude scale is 0..512 (fade = `512*t/fadeIn`).
`off = ((t % period) * n) / period`. Render (0x43e0c0), per output row y,
`k = (off + y) mod n`, `d = T[k]*A >> 9`:
`work(x,y) = base(x+d, y+d)` (both axes shift by the same d → diagonal
shimmer). Out of range → bg colour if fill, else unchanged.

### 5.4 bg_zoom / bg_rot
bg_zoom init (0x43d100) args: `cx, cy, zoomStr, easeStr, D`. Target view
(`FUN_0043cec0`): `w = trunc(W/z)`, `h = trunc(H/z)` (W,H = layer display
size), `x = clamp(cx - w/2, 0, imgW - w)` (w > imgW → x=0, w=imgW), same for y.
The start view is the current source rect (or the whole image). run:
`t' = ease(t)` (§1.3), `p = (t' % D)/D` (p in [0.999,1.001] → 1),
`rect_i = trunc(start_i + (target_i - start_i)*p)`, then
`FUN_0043ab90(layer, …, rect)` (the view rect is stretched to the display).
When t ≥ D the target is applied and the effect ends (returns 1). D==0 → apply at once.
bg_rot init (0x43d520) args: `cx, cy, angle(512/turn), zoomStr (≠0), easeStr, D`.
The start values come from the layer (+0x64,+0x68 centre, +0x84 angle, +0x88
zoom; default 0,0,0,1.0). run interpolates the same way and calls
`FUN_0043af80(layer, cx, cy, angle, zoom)`.

### 5.5 bg_blur (zoom/rotation accumulation blur)
init (0x43eae0). Numeric form `args[0]==0`:
`cx, cy, z0Str, a0Str, z1Str, a1Str, D, opa, easeStr, imageName|0, flipV, flipH, keep, smooth, prio, autoRemove`
(z = zoom double, a = angle 512/turn). Key-frame form `args[0]="(x,y,z)(x,y,z)…"`:
`str, spline, D, opa, easeStr, image, flipV, flipH, keep, smooth, prio, autoRemove`
(spline ≥3 keys → `FUN_0043e870`/`0x43efc0`, otherwise linear 0x43f0a0). The
overlay = the named ELG, or a copy of the layer's base image (+0x2c surface),
optionally flipped. Each frame (≥16 ms after start, `p = ease(t/D)`):
* rotating variant (0x43ee50): `z=z0+(z1-z0)p`, `a=trunc(a0+(a1-a0)p)`,
  matrix `m=(c·z, −s·z, s·z, c·z, cx−(cx·c+cy·s)·z, cy+(cx·s−cy·c)·z)`
  (c=dcos[a], s=dcos[a−128]) → DLL affine blit +0x30 of the overlay **onto the
  current work surface** with opacity `opa` (e.g. 64/128).
* non-rotating (0x43f120): DLL stretch +0x28 of the overlay to a rect
  scaled by z around (cx,cy) [likely], opacity opa.
Because each frame blends onto the previous result, the image gets a radial
zoom-blur trail. At D: final draw with p=1, then remove (autoRemove) or hold.
`keep` (+0xf8) draws onto the base image instead of the work surface.

### 5.6 bg_move / sp_move (move language)
init (bg 0x403b50): `moveStr, stepMs, snapMode(0xffff=none), snapshot, keep`.
The parser `FUN_00401bd0` compiles the string into 32-byte steps. Tokens
(spaces ignored):
* number N → set step duration to N ms (op −8); `L` → loop to start (−7);
* `a` → linear (−1); `aN`, `a+N`, `a-N` → easing exponent ±N (op −2, §1.3);
* `i` / `o` → fade in / fade out (end opacity 255 / 0); `b` / `f` → put the
  layer behind / in front (flag 2 / 1);
* position moves (one step each, can be combined in a token run): for BG
  layers (cur = scroll offset x,y; W,H = screen; iw,ih = image)
  `l` x→0, `r` x→iw−W, `c` centre ((iw−W)/2,(ih−H)/2), `u` y→0, `d` y→ih−H,
  `!l/!r/!u/!d/!c` = the same just outside the screen; sprites use
  screen-relative targets (`l` = centre − W/4, `r` = centre + W/4, …);
  `<n` / `>n` x −/+ 25·n, `-n` / `+n` y −/+ 25·n (n defaults to 1);
  `$n` width +50n centred, `[n` grow to the left, `]n` to the right;
  `%n` height +50n centred, `^n` up, `_n` down.
run (0x403900): plays the steps in order; the move interpolation is
`FUN_00403db0(step, t')` with t' eased if an exponent is active.
Examples: `"r"`, `"a0 r"`, `"a0 $ $"`, `"l"`.

### 5.7 quake (layer)
init (0x4046b0) args: `D(−1 endless), k, ampX, ampY, decayStr, snapshot,
bgFlag, fgFlag, auto`. It shakes the layer and a linked second layer by
changing their image offsets (+0x48/+0x4c), with the same RNG/decay formula as
the transition quake (`k≠0` gives a different, damped formula [likely]).

### 5.8 noise / paper / light / snow / rain (overlay particles)
* **noise** (0x404dc0): args `opacity, z, showFront, showBack`. Loads
  `SYS\NOISE.ELG` (800×600 8-bit gray) and creates a screen-sized 32-bit surface
  shown by two sprites (one per page tree) at priority z (1993) with the given
  opacity. It re-runs every 100 ms (scheduler interval 100): copy the noise
  pixels starting at a random linear offset `r % (W·H)` (wrapping) into the
  surface. Film-grain flicker.
* **paper** (0x405af0) `count, zBase, zStep, prewarm, showFront, showBack`:
  loads `SYS\PAPER_0..7.ELG` (0–3: 192×8 = 24 frames of 8×8; 4–7: 384×16 = 24
  frames of 16×16). Each particle picks image k=r&7, z = zBase+(4−k)·zStep.
  Particle simulation in `FUN_004050c0/00405320`, run 0x405a40.
* **light** (0x406a00): `SYS\GR_LT0..2.ELG` (80×16, 120×24, 160×32 strips of
  5 frames [likely]), k=r%3.
* **snow** (0x4076c0): `SYS\SNOW_0..4.ELG` (6×6 … 14×14 soft dots), k=r%5.
* **rain** (0x408270): no images (6 procedural layers, k=r%6,
  z = base+(2−k)·step).
The FUNC_LIB call is `(count, 1993, 100, prewarm, f1, f2)`. Counts used: 20–80.

### 5.9 bg_anime
init (0x489390) args: `animData (from N_1fe(file)), basePath "ANIME", start,
?, loop, …`. Frame list of 16-byte entries `{int frame (<0 = command), u32
durationMs, …}`. In catch-up mode (`DAT_00503f54`) several frames are skipped
when late. It is used only through FUNC_LIB 0xdec8.

---------------------------------------------------------------------------

## 6. Image natives N_13b..N_18c (used ones)
All unwrap arg0 to an image surface (type 4 = image itself; 7/9 → +0x30
object's surface; 8 → +0x54; 10 → +0x30). Rect args: four ints
`l,t,r,b`, with `0xffff` in the first one meaning "whole image".
Surface fields: +4 w, +8 h, +0xc pitch, +0x3c pixels, +0x40 bpp (8/32), +0x44
palette (8-bit), +0x51 dirty, +0x58 dirty-rect list.

| native | addr → impl | args | effect |
|---|---|---|---|
| N_13b | 428fe0 → 441670 | (img, file, x, y, flag) | decode an ELG file into the existing image at (x,y) |
| N_13e | 4291b0 → 440b30 | (img, argb, rect…) | fill ARGB |
| N_13f | 429280 → 440c00 | (img, r, g, b, rect…) | fill RGB, keep alpha |
| N_148 | 429880 → 441df0 | (dst, src, dx, dy, rect…) | copy (incl. alpha; src opacity / 8-bit palette handled) |
| N_14c | 429e80 → 442df0 | (dst, src, dx, dy, rect…) | alpha blend using src alpha (DLL blit method 2, opa 255); C path `FUN_004b4c90`: `d = s·a/256 + d·(256−a)/256` saturated (a=255→256) |
| N_150 | 42a480 → 443610 | (dst32, src8, dx, dy, rect…) | copy the 8-bit plane into dst's **alpha** byte (`FUN_004b56b0`) |
| N_154 | 42aa80 → 443d20 | (dst, src, dx, dy, rect…) | copy RGB, keep dst alpha (`FUN_004b58a0`) |
| N_15a | 42b280 → 440da0 | (img, L (0..510), rect…) | brightness: L≤255 `c=mul[L][c]`; L>255, u=L−255: `c = c·(255−u)/255 + u` |
| N_16a | 42c770 → 4b8040 | (dst, src, opa, dx, dy, rect…) | straight-alpha source-over composition including output coverage (src alpha × opa/255; DLL method 3; C fallback 4692a0) |
| N_175 | 42d930 → 4b90b0 | (img, rect…) | grayscale. 32 bpp: `Y=(B·77+G·150+R·29)>>8` (byte order as in the code: bytes 0/1/2 weighted 0x4d/0x96/0x1d). 8 bpp palette: `(R·77+G·150+B·29)>>8` |
| N_176 | 42d9f0 → 4b9340 | (img, mR, mG, mB, aR, aG, aB, rect…) | `c' = clamp(add + (c·mul >> 7))`, mul ≤ 256 (128=1.0), add −255..255; alpha kept |
| N_177 | 42dac0 → 4b9650 | (img, mode, rect…) | invert: 0 RGB (xor 0xffffff), 1 ARGB, ≥2 alpha only |
| N_17b | 42de10 → 4b9e90 | (img8, L, rect…) | N_15a on 8-bit images (masks) |
| N_185 | 42e910 → 495c20 | (img, rx, ry, alphaFlag, rect…) | box blur (DLL +0x34) |
| N_186 | 42e9f0 → 495ca0 | (img, rect…) | vertical flip (DLL +0x38/+0x3c) |
| N_187 | 42eab0 → 495d10 | (img, rect…) | horizontal flip (DLL +0x40/+0x44) |
| N_18c | 42eed0 → 495f70 | (dst, s1, s2, rule, opa, vague, mode, [dx,dy,w,h,s1x,s1y,s2x,s2y,rx,ry]) | direct rule blend (DLL +0x68), used by title2 |

(The other N_13c..N_18b entries exist but are not called by the scripts.)

---------------------------------------------------------------------------

## 7. Implementation checklist (CPU compositor)
1. Stage keeps two pages plus a transition frame buffer. Start = record
   transition, mode, flag. Per frame: update(now) → render(ctx{old=front
   composite, new=back composite}) → if done, swap/copy pages.
2. Implement cross, rule (table + switch), scroll, wave, mosaic, rotaxis,
   trans#86, quake, zoomflip, rotcenter, addsub, hblur exactly as in §2.3. Tone
   and the other 86 trans patterns are not needed for this game.
3. FUNC_LIB transitions are asynchronous (G[829]); the next visual change or
   skip forces them to finish.
4. Layer effects = per-layer tasks writing into the layer work surface:
   bg_haze/bg_rast/bg_blur write pixels; bg_zoom/bg_rot/bg_move change the
   view; noise/paper/light/snow/rain add sprites.

## Open questions
* Exact math of the rotaxis quad filler `FUN_004600f0` (sub-pixel rules, edge
  inclusivity) and of the stretch/affine filters in tvpgl (linear vs nearest
  sampling for each flag value) are only [likely].
* The stage live flag (+0xbc) — who sets it, and whether page animations keep
  running during transitions in the shipped config.
* bg_blur non-rotating rect formula (0x43f120) and the spline path (0x43e870,
  0x43efc0) are not fully derived.
* Particle physics for paper/light/snow/rain (0x4050c0, 0x405320, 0x405ee0,
  0x406e00, 0x407ab0 and the run functions) not analysed: speeds, sway,
  frame stepping.
* Layer quake with k≠0 (0x404240 second branch) and the "linked second layer"
  identity.
* Move language: the exact meaning of the four size fields per step and
  `FUN_00403db0` interpolation; sprite-relative targets for sp_move.
* bg_anime data format (`N_1fe`) and the 87 trans patterns other than #86.
* Order of the strings in `G[881]` / `G[884]` (taken from the USER_VAR byte
  order; not checked against the VM's initialiser).
* The MMX/SSE variants chosen at run time may round differently from the C
  reference by ±1.
