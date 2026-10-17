# Audio, movie, input and the sound manager (DAT_00503630)

ORE_TUBA.EXE 1.01. All addresses are VAs in the EXE. "args[i]" = i-th pushed
native argument (source order). `app` = DAT_00503248, `eng` = script engine
object (0x404 bytes, FUN_004154b0, stored at app+0x19c8).
Tags: **[V]** verified in code, **[I]** inferred/likely, **[?]** guess.

---------------------------------------------------------------------------
## 0. Global map (correction of the task's assumptions)

| global | what it is | set at |
|---|---|---|
| DAT_00503630 | **the sound manager ("SM")** = eng+0x138 (embedded, vtable 0x4ee634, ctor FUN_00452440). Owns the 2 BGM streams and the N sound channels (SE / system SE / voice). Natives N_285..N_2a5 | 0x4154b0 tail: `DAT_00503630 = _DAT_005034b0 = eng+0x138` [V] |
| DAT_00503208 | movie object, DS_MOVIE.DLL path ("window/fullscreen" movie, 0x30 bytes, vt 0x4eee44) | N_271 [V] |
| DAT_0050320c | movie object, DS_SPRITE.DLL path (movie rendered as a layer, 0x124 bytes) | N_271 when app+0xd27!=0 [V] |
| DAT_00503210 | movie object, DS_PRIM.DLL path (0x58 bytes; DS_PRIM.DLL is **not shipped**) | N_271 when app+0xd27==0 [V] |
| DAT_00503228 = DAT_00503f40 | DirectSound device wrapper (app+0x1920); also keeps a list (+0x20 head,+0x24 count) of "loose" sound objects created by N_254..N_258 | 0x413000 area [V] |
| DAT_00503218 | task (timer) manager app+0x19a8; ticked every main-loop iteration with GetTickCount()/timeGetTime() ms (`vt+0x30(now)`, 0x4016b0) . All audio fades are tasks in it. SM+0x208 points to it | [V] |
| DAT_00503254 | keyboard device (app+0xbec → input mgr+8) | [V] |
| DAT_00503250 | mouse device (app+0xbf0 → input mgr+0xc) | [V] |
| DAT_00503230 | joystick #0 device (input mgr vector +0x14) | [V] |
| app+0x1904 | logical-button map (20 logical buttons → device inputs), natives N_220..N_232 | [V] |
| app+0xbe4 | input manager; polled once per frame FUN_00435ea0 (keyboard vt+0x28, mouse vt+0x28 + FUN_0044a250, joysticks vt+0x20) | [V] |

---------------------------------------------------------------------------
## 1. Audio

### 1.1 Backend
* DirectSound, secondary buffers created with DSBCAPS_CTRLPAN|CTRLVOLUME|CTRLFREQUENCY
  (0xe0) [+GLOBALFOCUS 0x8000 if device+5, +LOCSOFTWARE if device+6]; fallback 0x80.
* Decoders through factory DAT_00503f44 (`vt+0x20(name,0,1,0)`): WAVE and Ogg
  Vorbis (wuvorbis.dll `wu_ov_*`). **`wu_ov_comment` is not imported → no
  LOOPSTART/LOOPLENGTH tags; looping is whole-file or intro+loop file pair.** [V]
* Two kinds of sound object (base class FUN_00480170, +4 = IDirectSoundBuffer*,
  +8 = volume 0..256):
  * **static** (vt 0x4ef470, 0x54 bytes, load FUN_00480d50): whole file decoded
    into one buffer. Used for SE, system SE, voice.
  * **stream** (vt 0x4efa8c, 0xd8 bytes, open FUN_00490140): 4-second ring buffer
    (size = 4*nAvgBytesPerSec), refilled by a thread (4 notify points) or polling.
    Used for BGM and optional "streamed" SE.
* Common vtable (offsets): +0x0c isStream, +0x10 close, +0x14 play, +0x18 playLoop,
  +0x1c pause (refcounted), +0x20 resume, +0x24 stop, +0x28 total PCM bytes,
  +0x2c total length ms, +0x30 seek(ms), +0x34 position ms, +0x38 isPlaying(&playPos,&writePos),
  +0x3c setVolume(0..256), +0x40 setPan(raw DS pan −10000..10000), +0x44 setFrequency(Hz),
  +0x48 adjust(dVol,dPan,dFreq), +0x4c get(vol,pan,freq), +0x50 getFormat(ch,rate,bits). [V]

### 1.2 Volume model [V]
`setVolume(v)` (FUN_00480230): v clamped to 0..256;
`dsVol = v<=0 ? -10000 : (int)(2000*log10(v/256))` (fyl2x·log10(2), ×2000, ftol).
=> **v is a linear amplitude factor v/256.** Port: `gain = v/256.0`.

Effective values:
* SE/voice channel: `v = ch.vol256 * ch.vol100 / 100` (FUN_00453430, int division).
  vol100 (0..100, ch+0x134) is the fade factor, vol256 (0..256, ch+0x138) the
  category/character volume.
* BGM: `v = stream.fadeVol(0..100, +0xe0) * SM.bgmVol(0..256, +0x1e0) / 100`
  (FUN_00452cf0).
* Movie (FUN_0046ea80/0046cda0/0046dee0): v 0..256,
  `dsVol = v<1 ? -10000 : -(int)((17.2 - v*0.0671875)^3)` (cubic curve, 0 at v=256,
  −50.88 dB at v=1). Port: `dB = -((17.2*(1-v/256))^3)/100`.
* Pan: raw DirectSound units; BGM pan SM+0x1e4 (N_289), channel pan ch+0x13c (N_297).

### 1.3 Sound manager layout (SM = DAT_00503630 = eng+0x138) [V]
| off | meaning |
|---|---|
| +0x004 / +0x0f0 | two BGM slots, 0xec bytes each (FUN_00453a30): stream object (0xd8) + `+0xd8 name` (main/loop file), `+0xdc name2` (intro file or NULL), `+0xe0 fadeVol` 0..100 (init 100), `+0xe4` pause count, `+0xe8` loop flag (byte) |
| +0x1dc | pointer to **current** BGM slot; +0x1e8 index 0/1; FUN_00453110 swaps (used by crossfade) |
| +0x1e0 | BGM volume 0..256 (config (3,5), init 0x100) |
| +0x1e4 | BGM pan |
| +0x1ec / +0x1f0 | channel count / channel pointer array (N_295) |
| +0x1f4 | default vol256 for category-1 channels (SE), −1 = keep |
| +0x1f8 | default vol256 for category-2 channels (system SE), −1 = use +0x1f4 |
| +0x1fc | default vol256 for category-4 channels (voice); −2 when VOICEINF exists (= never applied, per-character volume used instead) |
| +0x200 | BGM enabled (config (3,1)) |
| +0x201 / +0x202 / +0x203 | category 1 / 2 / 4 enabled (config (3,2)/(3,3)/(3,4)) |
| +0x204 | active BGM fade type: 0 none, 1 vol fade, 2 fade-in, 3 fade-out, 4 crossfade |
| +0x208 | task manager (DAT_00503218) |
| +0x20c | BGM duck task (id 0x2713) or 0 |
| +0x210 | critical section |
| +0x234 | pointer to `eng` (for VOICEINF table, eng+0xfc..0x10c) |

Channel (0x148 bytes, created by N_295 / FUN_004531f0):
| off | meaning |
|---|---|
| +0x000 | active sound object (= &ch+4 stream or &ch+0xe0 static) or 0 |
| +0x004 | stream object; +0xdc its file-name copy |
| +0x0e0 | static object (its file name at +0x120 = obj+0x40) |
| +0x134 | vol100 (init 100) |
| +0x138 | vol256 (init 0x100) |
| +0x13c | pan (init 0) |
| +0x140 | play state: 0 stopped, 1 played once, 2 looping (not reset when a one-shot ends) |
| +0x141 | fade state: 0 none, 1 volume fade (0x2714 N_29e), 2 fade-in, 3 fade-out |
| +0x142 | category mask: 1 = SE (default), 2 = system SE, 4 = voice |
| +0x144 | running fade task or 0 |

### 1.4 Channel allocation used by the game [V] (gameinit.sob, func_lib)
`N_295(12)` → 12 channels. `N_298(i,2)` for i=0..4, `N_298(5,4)`.
| ch | cat | use |
|---|---|---|
| 0 | 2 | SE\SE_OK.OGG (preloaded by gameinit) |
| 1 | 2 | SE\SE_OK2.OGG |
| 2 | 2 | SE\SE_CANCL.OGG |
| 3 | 2 | SE\SE_SEL.OGG |
| 4 | 2 | SE\SE_SEL2.OGG (history.sob) |
| 5 | 4 | voice |
| 6 | 1 | looping ambient SE (lib L+20708 / L+20870 / L+209a4 / L+20a4c, name saved in G[851], stream flag G[852]) |
| 7 | 1 | per-message SE (G[850], loaded+played at message display, stopped with N_29b(7) when the message advances); op.sob uses it for the OP song |
| 8..11 | 1 | one-shot SE; slot argument n → ch 8+n, n<0 → round robin G[853]=0..3 |
System SE are played by scripts as `N_29c(ch,0)` with ch 0..4.

### 1.5 BGM [V]
Script entry: lib **L+1f600(no, fadeMs, loop=1)** (2115 calls).
* return if G[905] or `no == G[904]` (current BGM number); G[904]=no.
* `no==0`: fadeMs ? N_290(fadeMs) fade-out : N_28b() stop.
* Names: if no ∈ {13,52,82,88} or no ≥ 91 → single file `BGM\BGM%02d.OGG`;
  otherwise intro+loop pair: name=`BGM\BGM%02dB.OGG` (loop body), name2=`BGM\BGM%02dA.OGG` (intro).
  (Files in BGM.LPK: bgmNNa/bgmNNb pairs, singles 13,52,82,88,91..99.)
* fadeMs>0 and previous no≠0 → **crossfade** `N_291(fadeMs, name, 1, name2, fadeMs, -1)`;
  fadeMs>0 and nothing before → `N_28f(fadeMs, name, 1, name2)` fade-in; fadeMs==0 → `N_28a(name,1,name2)`.
* Typical scene call: `L+1f600(5, 1000, 1)`, stop: `L+1f600(0, 500, 1)`.
* L+1fad4(wait): waits (yield loop) while N_293()!=0 (fade active); aborted by
  G[915]|G[914]|N_9(0,0)|N_234 (Ctrl/Shift skip) → then N_28b if G[904]==0.
  L+1fc40: waits while N_292() (BGM playing), then G[904]=0, N_28b.

Playback semantics (FUN_00452db0 open/play, stream thread 0x491140/0x4912b0):
* `open(stream, fileA=name2, fileB=name)`: plays A once, then B. When B hits EOF and
  loop flag set → B.seek(0) (A is never replayed). Single file: loops the whole file.
  Non-loop: plays to end, then stops.
* If the same name (and name2) is already playing → returns −1, no restart.
* If BGM disabled (SM+0x200==0): names are stored, returns −2; enabling BGM later
  (FUN_00452a50) re-opens and starts the stored BGM (if pause count 0). Disabling stops
  both slots (names kept).

Fade tasks (all linear, in the 0..100 fadeVol domain, ticked with ms timestamps):
| native | task (id) | behaviour |
|---|---|---|
| N_28e(target,ms) | 0x4ba1d0 (0x2710), unused | fadeVol: cur → target over ms |
| N_28f(ms,name,loop,name2) | 0x4ba360/0x4ba400 (0x2710) | if same BGM already playing → −1. Save cur fadeVol F, set fadeVol 0, start BGM, ramp 0→F over ms. SM+0x204=2 |
| N_290(ms) | 0x4ba560/0x4ba600 (0x2710) | if not playing → stop, −1. ramp F→0 over ms, then stop (FUN_00453000) and restore fadeVol=F. SM+0x204=3 |
| N_291(D,name,loop,name2,O,vol) | 0x4ba6f0/0x4ba850 (0x2711) | crossfade, see below. SM+0x204=4 |
Starting any BGM fade first removes tasks 0x2710 and 0x2711 (manager vt+0xc(id) = finish).
Crossfade (FUN_004ba730): old slot fades F_old→0 over D ms starting at t=0 and is
stopped at t=D; the new slot (opened into the other BGM slot) starts at
t = D−min(O,D), fading 0→T over D ms (T = vol arg 0..100 or −1 → F_old); task ends at
2D−min(O,D) (or when new stream stops); finish swaps current slot to the new one.
The lib always passes O=D (both fades simultaneous, total D ms).

BGM ducking under voice — N_2a5(ch, ratioStr) (task 0x2713, FUN_004bacb0/FUN_004bab50) [V]:
* ratio = atof(ratioStr) or, if NULL, config (3,14) `app+0xea9`: 50 → −1.0, else value*0.01.
  ratio must be <1.0; ratio<0 → target = V>>1, else target = (int)(V*ratio); V = SM+0x1e0.
* Requires channel ch playing and BGM playing.
* States: 0 start; 1: V → target linearly over **500 ms**; 2: wait until channel ch stops;
  3: hold **1000 ms**; 4: target → V linearly over **1000 ms**; done → V restored.
  Aborts (restores V) if BGM stops. A new duck while an old one is in state 2/3 jumps
  directly to state 2 (stays ducked). Changing BGM volume via N_288 kills the duck task.
* Called by lib after a voice starts when config (3,10) (`ea5`) && N_292(); also history.sob.

### 1.6 SE (lib wrappers) [V]
* `L+1ff58(name, slot, stream)` (1149 calls): return 0 if G[905]|G[915]|G[914]|!config(3,2)|N_234()
  (**no SE while skip key held**); ch = alloc(slot) (8+slot or round robin 8..11);
  `stream ? N_29a(ch,name) : N_299(ch,name)`; `N_29c(ch,0)`. Scene calls: `("SE\SE051.OGG",0,0)`.
* 0x200d8: same without skip check; 0x201d8/0x203e0: play and wait length
  (N_265(N_2a3(ch)) ms via wait helper @1c684).
* `L+205d0(n)`: N_29d(8+n). 0x20638: N_29b(8..11), G[853]=0.
* Ambient loop ch 6: `L+20708(name, stream)` → load, `N_29c(6,1)`;
  `L+20870(name, ms=1000 if 0, stream)` → `N_29f(ms,name,6,1,stream)`;
  `L+209a4(ms=1000 if 0)` → `N_2a0(ms,6)`; `L+20a4c()` → `N_29d(6)`.
* Channel fades: N_29f fade-in (task 0x2714): optional load, vol100:=0, play, ramp
  0→old vol100 over ms; N_2a0 fade-out: vol100 old→0 over ms, then stop and restore vol100.
  Both stop early if the sound stops. ch+0x141 = 2/3 while active.
* 0x20aa4/0x20c90: wait for channel fade end / channel end, skippable (stops channel)
  by G[915]|G[914]|N_9(0,0)|N_234.

### 1.7 Voice [V]
VOICEINF (SCRIPT\VOICEINF.DAT, loaded by FUN_00424440 into eng):
```
u8 n; char ext[n]                       ; "OGG\0" -> eng+0x10c
u32 nPrefix (99)                        ; eng+0x104, table eng+0x108: {char* prefix, int charIdx}
u32 nChar (34)                          ; eng+0xfc, table eng+0x100 (16 bytes each)
per char: u8 n; char name[n] (cp932)    ; +0  display name
          u8 m; if m: +0xd=1; if m>1: char str[m] -> +4 ; skip m bytes
          u32 k; k * {u8 n; char prefix[n]}   ; e.g. "ASU","MIY","KOB"...
```
Char entry: +0 name, +4 alt name, +8 **volume 0..256**, +0xc **enabled**, +0xd flag
(set 1 at load if m≠0, cleared to 0 when that character's voice is played; read via
N_b(i,2)/N_b(i,5)) [I: "voice heard/unlocked"]. Set by N_a(i, field 0/1/2, value) from
config2.sob (per-character volume slider, per-character on/off).

N_2a4(ch, name, n, list[n]) → FUN_004326a0:
1. find the first prefix p with `name` starting with p (case-sensitive) → prefix idx k,
   char c = prefix[k].charIdx; not found → −3; no table → −5.
2. ch == −1 → return k; ch < −1 → return c.
3. if voice enabled (SM+0x203) and char c enabled (+0xc): j = index of k in list (or −1);
   load `wsprintf("VOICE\%s.%s", name, ext)` as **static** into channel ch with
   lip-sync analysis flag = (j≥0 && config(0,5) app+0xe48) ? 2 : 0; set ch.vol256 = char.volume;
   return (n>0 ? j : k). Load failure → −4. Voice/char disabled → −2. Does not play.
Message flow (lib L+2c3c, 0x5630): if *G[847] (voice name) and config(3,4):
`r = N_2a4(5, G[847], 6, G[775..780])` (G[775..780] = prefix indices of shown characters);
if r ≥ −1: pan=0; if r≥0 && config(3,11): x = G[777+r] (0..800):
`pan = x<=0 ? -10000 : x<400 ? x*10000/400-10000 : x<800 ? (x-400)*10000/400 : 10000`;
`N_297(5,pan)`; `N_29c(5,0)`; if r≥0 && G[827+r] && config(0,5): N_d1(13002), lip-sync
`N_214(G[827+r], N_2a3(5))`; if config(3,10) && N_292(): `N_2a5(5,0)` (duck);
if config(3,13): G[846]=1 … (auto-mode wait helper).
Voice stop on advance — lib L+0(arg), called when the message is dismissed (0x6628):
if *G[847]: if !arg && config(3,9) (`app+0xea4`) → `N_29d(5)`; then *G[847]=0.
(Auto mode waits for `N_2a1(5)==0` before advancing, 0x652c.)
Other voice helpers: L+20e64(name): N_2a4(5,name,0) and N_29c(5,0) (no pan/duck);
0x20f64: same and wait the voice length.

Lip-sync data [V]: with flag 2, FUN_00480d50 keeps an envelope copy of the PCM at
obj+0x50: each 32-sample block replaced by its mean |sample| (8-bit: |s−0x80|);
obj+0x4c = peak/4, obj+0x4a = 1 (8-bit) / 2 (16-bit). FUN_004813c0(bytePos, n) =
`min(n-1, |env[pos]|*n/(peak/4))` (n==0 → raw 0..255). The N_214 task (vt 0x4edec4,
id 0x32ca) each tick reads the play cursor (vt+0x38) and switches the mouth frame
when the level changes (FUN_004580f0). Also exposed as N_26e/N_26f.

### 1.8 Config ↔ audio (N_8/N_9 category 3; applied by FUN_0040b8f0 at start) [V unless noted]
| item | field | meaning |
|---|---|---|
| (3,1) | e8d | BGM on → SM+0x200 |
| (3,2) | e8e | SE on → SM+0x201 |
| (3,3) | e8f | system SE on → SM+0x202 |
| (3,4) | e90 | voice on → SM+0x203 |
| (3,5) | e94 | BGM volume 0..256 → SM+0x1e0 |
| (3,6) | e98 | SE volume → SM+0x1f4 |
| (3,7) | e9c | system SE volume → SM+0x1f8 |
| (3,8) | ea0 | voice volume (only if no VOICEINF; game uses per-character N_a) |
| (3,9) | ea4 | stop voice when message advances |
| (3,10) | ea5 | duck BGM during voice |
| (3,11) | ea6 | pan voice by character x position |
| (3,13) | ea8 | [I] voice-related auto/wait option |
| (3,14) | ea9 | duck ratio in % (50 = half) |
| (3,15) | e91 | [I] common volume for all characters (config2 loops N_a over 34 chars) |
| (0,5) | e48 | lip-sync on |
| (6,5) | ed4 | movie volume 0..256 |
| (6,8) | eda | [I] movie in separate/fullscreen DS_MOVIE window |
Config UI (config2.sob) on change: N_285(0,v,0) BGM; N_285(1,v,1) SE (+0x201, pauses/
restarts looping cat-1 channels); N_285(2,1,v) sys SE; N_285(2,2,v) voice;
N_288(−1,v) BGM vol; N_287(v,0|1,mask) channel vols; test sounds via N_254 ("SE/SE_TEST.OGG",
"VOICE/%s_TEST.OGG" with N_b(char,6) = prefix) + N_259/N_268/N_25d.

### 1.9 Save state of the SM (vtable 0x4ee634: +4 size, +8 save 0x452670, +0xc load 0x4528c0) [V]
Saved: BGM pan, BGM name, name2, fadeVol, loop flag; channel count; per channel: kind
(1 stream name / 2 static name / 0 none), name, (static: obj+0x4a), vol100, pan,
looping(state==2), category. Load: stops BGM, reopens it (FUN_00452db0), recreates
channels, reloads names, restarts looping channels. (Note: the save code writes
`+0xd8` twice for name/name2 [V, likely bug]; G[851]/G[852]/G[904] script globals are
saved separately.)

---------------------------------------------------------------------------
## 2. Movies [V]
* Files: DATA.LPK `DATA/OP1.MPG` (idx 0), `DATA/OP2.MPG` (idx 1), table @0xc66 in
  movieplay.sob. Played by script "MoviePlay" with G[912] = idx (s00_00 after "OP",
  s04_06j2 sets idx 1).
* DLL interfaces: DS_MOVIE.DLL exports GetDllVersion (must return 0x10000) and
  GetMovieObject(out obj, hwnd/surface, IStream*, type, path, …); DS_SPRITE.DLL exports
  GetDShowGdiSpriteCtrl. Both are DirectShow wrappers; type from extension
  (mpg/mpeg→0, avi→3, wmv/asf/wma→6, mpv→1, dat→2, mov/mp4→4). Files come from LPK
  through an IStream (resolver vt+0x44/vt+4).
* N_271(name, windowMode, prio): stops any movie (N_272); windowMode≠0 → DS_MOVIE in
  the game window (DAT_00503208, app+0xda8 = state+1 suspends normal rendering);
  else DS_SPRITE layer movie (DAT_0050320c) inserted in the layer list
  (layer mgr+0x110 …, priority args[2]). Returns 0 ok / error.
* movieplay.sob flow:
  1. gate N_5(4) && N_9(6,4) else mark seen G[idx]=1 and return;
  2. `N_29b(ch)` for ch 5..11 (unload voice + all SE; BGM untouched);
  3. fs = N_9(6,8); if fs and N_6(22)≠1 → N_3(2,1) (switch screen mode, restored after);
  4. `N_271(name, fs, 0)`; rect: N_cd()&&fs ? client rect (N_ce) : `N_278(0,0,800,600)`;
  5. `N_27f(N_9(6,5))` volume; `N_274()` play;
  6. loop (one `end` per frame): stop if **Esc** edge (N_235(1)) always; if already seen
     (G[idx]) also on OK/Cancel logical buttons (N_227/N_228) or left/right click
     (N_242/N_243); stop when `N_27e()==0` (DirectShow State_Stopped);
  7. `N_272()`; restore screen, call L+1cad0.
* Time unit conversion: N_279/N_27c/N_27d use ms (REFERENCE_TIME/10000).
Port: decode MPEG-1 with pl_mpeg, letterbox/scale to the 800×600 game view, audio
through own mixer with the cubic movie-volume curve, skip rules as above.

---------------------------------------------------------------------------
## 3. Input

### 3.1 Devices [V]
Input manager app+0xbe4 (FUN_00435800 creates devices): DirectInput keyboard
(FUN_0046b050, vt 0x4eecd0) or message-based keyboard (FUN_0046b370, VK→DIK table
0x4eeba0); DirectInput mouse (FUN_0044a3a0, vt 0x4ee4f0) or message mice
(0x44a640/0x44a6f0); joysticks via winmm/DirectInput.
**Keyboard** (DIK scan codes): two 256-byte state buffers at +5/+0x105, +4 = index (double
buffered, current = idx^1). vt+4(dik) held; vt+8(dik) pressed this frame (cur & !prev);
vt+0xc first held key; vt+0x10 first pressed key; vt+0x14 clear; vt+0x18(dik) auto-repeat
(message keyboard only: bit 0x20 of state; DirectInput returns 0). +0x205/0x206/0x207 =
ScrollLock/CapsLock/NumLock toggle state (GetKeyState). Key repeat for scripts is
therefore **not** provided; scripts only use held/edge.
**Mouse**: DIMOUSESTATE ×2 at +8/+0x18 {lX,lY,lZ,buttons[4]}, +4 index.
code 0 left, 1 right, 2 middle, 3 wheel up (lZ>0), 4 wheel down (lZ<0).
vt+4(code) held, vt+8(code) edge (wheel: prev lZ==0 && cur lZ≷0), vt+0xc/+0x10 first
held/edge code, vt+0x18 current lZ. Buttons are ignored when the cursor pointer
(+0x28 → app+0x12c) is outside the window (x<0||y<0).
**Cursor position**: FUN_00436220 each frame: GetCursorPos → client → scaled to game
coords (app+0xcf4 × app+0xcf6 = 800×600) → app+0x12c/0x130, −1/−1 if outside or
minimized. app+0x134 = edge code (8 top, 2 bottom, 4 left, 6 right, 7/9/1/3 corners, 0
inside) – also drives the auto-hide menu bar in fullscreen.
**Cursor shape** FUN_00449f30: 0..12 → IDC_ARROW, CROSS, HELP, IBEAM, NO, SIZEALL,
SIZENESW, SIZENS, SIZENWSE, SIZEWE, UPARROW, APPSTARTING, WAIT; ≥13 → EXE cursor
resource id shape+0x6b (120..123 exist). Optional sprite (animated layer) cursor
FUN_0044a100 (N_23d, unused). Show/hide = message 0x9000 to the window (ShowCursor loop).

### 3.2 Logical buttons (app+0x1904, 20 entries; device 0 kbd, 1 mouse, 2+n joystick n) [V]
Defaults (0x413150..): 0 Up: DIK_UP, joy0 code0; 1 Down: DIK_DOWN, joy code1;
2 Left: DIK_LEFT, joy code2; 3 Right: DIK_RIGHT, joy code3; 4 OK: DIK_RETURN, DIK_NUMPADENTER;
5 Cancel: DIK_ESCAPE; 0x12: DIK_PRIOR (PgUp); 0x13: DIK_NEXT (PgDn).
Config overrides (FUN_0040b9c0/FUN_0040bb90): keyboard key for 4 = (4,2) eaa, for 5 = eab,
for 6..17 = eac..eb7; joystick buttons for 4..17 = ebb..ec8 ((5,6..)); mouse right
button (code 1) → logical by (5,1) ec9: 0 none,1→0xc,2→0xb,3→6,4→0xf,5→7,6→8;
wheel up (code 3) → (5,2) eca ? 0x12 : 0xe; middle (code 2) → (5,4) ecc (same table);
wheel down (code 4) → 0x13 if (5,5) ecd. (5,0) eba = joystick enabled.
Natives: N_220(i) edge, N_221(i) held, N_222(first,count) first edge-pressed logical
index ≥first (−1 none), N_227/N_228 = edge OK/Cancel, N_229..22c edge Up/Down/Left/Right,
N_22d..232 held versions, N_224/225/226 add/remove/replace binding (unused).
System key dispatch is lib L+d0c (N_222(6,0) → command = index−6; F1..F5 (DIK 59..63)
without Alt: with skip-modifier → quick save slot, else quick load; right click
handled when config (5,1)≠2 && (5,3)) [I for exact command meanings].

### 3.3 Message advance / skip [V]
* Advance on N_242 (left click edge) || N_227 (OK) || N_220(0x13) (PgDn / wheel-down if
  mapped) (lib 0x62c4). Auto mode: config (1,7)>0 waits voice end N_2a1(5).
* **Skip key = N_234()**: held state of Shift (config (4,0)=0, DIK 0x2a/0x36) or Ctrl
  ((4,0)=1, DIK 0x1d/0x9d). Used 105× (skip text, suppress SE, abort waits).
* After advancing, if config (0,4) and not at a window edge (N_241()==0): hide cursor
  (G[845]=1, N_23e(0)); L+a40 re-shows it when the mouse moves (N_23f vs G[842]/G[843]),
  and opens the bottom panel when y ≥ 578 and config (0,9) [I].
* Script DIK usage: 1 Esc, 56/184 L/R Alt, 59..63 F1..F5, 199/201/207/209 Home/PgUp/
  End/PgDn (history), plus key variables in title/scskip.

---------------------------------------------------------------------------
## 4. Native reference

### 4.1 N_234..N_268 (used ones marked *)
| N | addr | args → ret | behaviour |
|---|---|---|---|
| 234* | 41af60 | () → bool | skip key held: (4,0)? Ctrl(0x1d/0x9d) : Shift(0x2a/0x36), kbd vt+4 |
| 233 | 41af00 | () → bool | same, edge (vt+8) |
| 235* | 41afc0 | (dik) → bool | key pressed this frame (kbd vt+8) |
| 236* | 41afe0 | (dik) → bool | key held (kbd vt+4) |
| 237/238/239 | 41b000.. | () | ScrollLock/Caps/NumLock toggle (kbd+0x205..7) |
| 23a | 41b030 | (b) | kbd+0x208 = b [?] |
| 23b | 41b050 | (dik) | key auto-repeat (kbd vt+0x18) |
| 23c* | 41b070 | (shape) | set cursor shape (§3.1); clears sprite cursor |
| 23d | 41b0d0 | (layer,frames,ms,pingpong,hotX,hotY) | animated sprite cursor (FUN_0044a100) |
| 23e* | 41b130 | (show) | show/hide cursor (FUN_0044a360, msg 0x9000) |
| 23f* | 41b160 | (&x,&y) | cursor pos in game coords (app+0x12c/0x130, −1 outside) |
| 240* | 41b190 | (x,y) | warp cursor to game coords (FUN_004360f0, scales in fullscreen); lib L+21230 animates in 6 frames |
| 241* | 41b1b0 | () → code | edge zone app+0x134 |
| 242*/243*/244* | 41b1c0.. | () → bool | left/right/middle click edge |
| 245*/246/247 | 41b220.. | () → bool | left/right/middle held |
| 248*/249* | 41b280/2b0 | (int* dz or 0) → bool | wheel up/down edge; *dz = lZ |
| 24a/24b/24c | 41b2e0.. | | joystick #0 info / edge / held (unused) |
| 24d* | 430f90 | (int*) | SendMessage(WM_COMMAND 0x9c52, args); if result>0 and app+0xe2e toggles option 0x4fbfc8 [?] (gameinit: N_24d(0)) |
| 253 | 4310b0 | () | free all loose sound objects |
| 254* | 4310c0 | (name, lipflag) → obj | new static sound from file (loose list) |
| 255* | 4310e0 | (obj) → obj | duplicate static buffer (stream → 2) |
| 256/257 | 431120/40 | (name[,name2]) → obj | new stream sound |
| 258* | 431160 | (&obj) | delete object, *obj=0 |
| 259*/25a | 431180/a0 | (obj) | play / play loop |
| 25b/25c | | (obj) | pause / resume |
| 25d* | 431200 | (obj) | stop |
| 25e/25f | | (obj) | task fades (unused) |
| 260/261/262 | | () | pause/resume/stop all loose objects |
| 263 | | (obj,ms) | seek |
| 264 | | (obj,&ch,&rate,&bits) | format |
| 265* | 431300 | (obj) → ms | total length ms (stream: intro+loop) |
| 266 | | (obj) → ms | position |
| 267 | | (obj,&pos,&wr) → bool | isPlaying |
| 268* | 431370 | (obj, vol 0..256) | set volume |
| 269/26a/26b/26c/26d | | | pan / freq / adjust / get / bytes |
| 26e/26f | | (obj,pos[,n]) | lip-sync level (§1.7) |

### 4.2 N_271..N_27f (movie; each checks 0050320c/00503210 first, then 00503208 under CS app+0x1b00)
| N | addr | args → ret | behaviour |
|---|---|---|---|
| 271* | 41c100 | (name, windowMode, prio) → err | open movie (§2) |
| 272* | 41c350 | () | release all movie objects, app+0xda8=0 |
| 273 | 41c400 | (loop) | loop flag |
| 274* | 41c480 | () → err | play (DS_MOVIE: app+0xda8 = mode+1) |
| 275 | 41c590 | () | stop |
| 276 | 41c690 | () | pause |
| 277 | 41c760 | () | resume |
| 278* | 41c870 | (l,t,r,b) | destination rect |
| 279 | 41c9a0 | () → ms | position |
| 27a/27b | 41cad0/cb90 | () → w/h | video size |
| 27c | 41cc50 | (ms) | seek |
| 27d | 41cd70 | () → ms | duration |
| 27e* | 41cea0 | () → state | 0 stopped, 1 paused, 2 running (init 3) |
| 27f* | 41cfa0 | (vol 0..256) | volume (cubic curve §1.2) |

### 4.3 N_285..N_2a5 (sound manager)
| N | addr | args → ret | behaviour (fields) |
|---|---|---|---|
| 285* | 4314f0 | (type,a,b) | 0: BGM enable(a) (+0x200, 452a50); 1: SE enable(a) with channel mask b (+0x201, 452bb0: stops/restarts **looping** channels of mask); 2: a=0 → +0x201 only, 1 → +0x202, 2 → +0x203 = b; 3: default vol a=0 +0x1f4, 1 +0x1f8, 2 +0x1fc = b |
| 286 | 4315b0 | (type,a) → v | getter of the above |
| 287* | 431630 | (vol256, onlyActive, mask) | set vol256 of channels in mask (if !onlyActive or looping or streamed) |
| 288* | 431660 | (fadeVol 0..100 or −1, bgmVol 0..256 or −1) | BGM volumes; bgmVol change kills duck task |
| 289 | 431680 | (pan) | BGM pan +0x1e4 |
| 28a* | 4316a0 | (name, loop, name2) | play BGM; loop==0xffff → (name, no name2, loop=1). ret −1 same, −2 disabled |
| 28b* | 4316e0 | () | stop BGM (kills tasks 0x2710/11/12), clear names |
| 28c/28d | | () | BGM pause/resume (count +0xe4) |
| 28e | 431710 | (target,ms) | BGM volume fade (unused) |
| 28f* | 4317b0 | (ms,name,loop,name2) | BGM fade-in |
| 290* | 431860 | (ms) | BGM fade-out then stop |
| 291* | 4318f0 | (D,name,loop,name2,O,vol) | crossfade; loop==0xffff → no name2, loop=1; O==0xffff → O=D |
| 292* | 4319e0 | () → 0..2 | number of BGM slots playing |
| 293* | 431a00 | () → type | BGM fade active (+0x204) |
| 294* | 431a10 | () → obj | current BGM stream object (for N_265 length; staff roll sr_*.sob) |
| 295* | 431a20 | (n) | (re)allocate n channels |
| 296 | 431a40 | (ch, vol100, vol256) | channel volume (−1 keep) |
| 297* | 431a60 | (ch, pan) | channel pan |
| 298* | 431a80 | (ch, mask) | channel category (mask&7 ≠ 0) |
| 299* | 431aa0 | (ch, name) | load static into channel (stops previous; applies category default vol, vol100=100) |
| 29a* | 431ac0 | (ch, name) | load streamed into channel |
| 29b* | 431ae0 | (ch) | stop + unload |
| 29c* | 431b00 | (ch, loop) | play (kills channel fade); if category disabled: not started, ret −1, but state=loop+1 so a looping sound starts when the category is re-enabled |
| 29d* | 431b20 | (ch) | stop (keep loaded), kill fade |
| 29e | 431b40 | (ch,target,ms) | channel volume fade (unused) |
| 29f* | 431bd0 | (ms,name,ch,loop,stream) | channel fade-in (name may be 0 = already loaded) |
| 2a0* | 431c80 | (ms,ch) | channel fade-out then stop |
| 2a1* | 431d10 | (ch) → bool | channel playing |
| 2a2* | 431d30 | (ch) → 0..3 | channel fade state +0x141 |
| 2a3* | 431d50 | (ch) → obj | channel's sound object (stream or static) or 0 |
| 2a4* | 431d80 | (ch, name, n, list…) → idx | load voice (§1.7) |
| 2a5* | 431da0 | (ch, ratioStr or 0) | duck BGM while ch plays (§1.5) |

Fade/task ids in DAT_00503218: 0x2710 BGM fade, 0x2711 crossfade, 0x2713 duck,
0x2714 channel fade, 0x32ca lip sync. Task update(nowMs) returns 1 when finished; manager
vt+4 add, vt+8 remove(task), vt+0xc finish-by-id.

---------------------------------------------------------------------------
## 5. Port notes
* Mixer: per-voice gain = v/256 (BGM: fadeVol*bgmVol/100/256), pan law of DirectSound
  (pan in hundredths of dB attenuation of the opposite side), movie: cubic curve.
* BGM = up to two decoders (intro A, loop B) per slot; two slots for crossfade.
* Fades/ducking are linear ramps driven by wall-clock ms; implement as per-frame tasks.
* Voice is fully decoded (static) to support lip-sync envelope (32-sample block means).
* Input: provide DIK-coded held/edge state per frame, mouse buttons 0..2 + wheel as
  codes 3/4, game-space cursor (−1 outside), edge code. Touch: tap = left click edge,
  two-finger tap / long press = right click (N_243), swipe = wheel; skip key (N_234) needs
  a UI toggle.

## 6. Open questions
1. Exact meaning of config items (3,0) e8c, (3,12) ea7, (3,13) ea8, (3,15) e91 and the
   N_d1(13002) call before lip-sync.
2. Char-entry +0xd ("heard"?) semantics and the second VOICEINF string (+4).
3. N_24d / WM_COMMAND 0x9c52 purpose; N_5(4), N_5(0) gates used by movieplay/L+21230.
4. Right-click command (@154 / @22fdc) and the full L+d0c command table (other doc).
5. Whether app+0xd27 is always set in the shipped config (DS_SPRITE path for in-window
   movies) – USER.CFG not decoded here.
6. Primary buffer format / output sample rate (not relevant for the port; secondary
   buffers take the file's format).
7. Save code writes BGM name1 twice (name2 lost?) – verify against a real save file.
