# 10 — Application lifecycle & system services (ORE_TUBA.EXE 1.01)

Scope: process start-up, config files, main loop / frame timing, virtual file
system (loose files + LPK), script loading details, save/load formats, and the
"system" natives N_3..N_67, N_a3..N_bf, N_1e9/N_1ea, N_220..N_232, N_2c7,
N_311..N_314. Read `00_vm.md` first.

Conventions: `app+X` = offset in the application object (`DAT_00503248`,
0x1b2c bytes, lives on WinMain's stack, vtable 0x4edbe8). `eng+X` = offset in the
script engine object (`*(app+0x19c8)`, 0x404 bytes, vtable 0x4ede4c). "args[i]" =
i-th pushed word. **[V]** = verified in code, **[G]** = guess / inference.

Some functions are missing from `build/re/ore_tuba.c` (Ghidra never created them):
FUN_0047aa50 (app init), FUN_0047cd60 (app exit), FUN_00401080 (threaded frame),
FUN_00413ae0 (config writer), FUN_0047e4f0 (timer tasks), FUN_004732b0,
FUN_00473580/473660 (VFS). They were decompiled for this doc with a throw-away
copy of the Ghidra project (`DumpDecomp`-style script that creates the function
first); use `x86dis.py ADDR` to re-check them.

---------------------------------------------------------------------------

## 0. TL;DR for the port

* Boot = read 4 small config files → mount PATCH*.LPK + SCRIPT.LPK → create
  window/DirectX → create script engine → load SOB_INIT.DAT, USER_VAR.SOB,
  FUNC_LIB.SOB, VOICEINF.DAT → run **MAIN.SOB**. MAIN calls GameInit (which
  mounts the other LPKs with keys and loads GAMEDATA.DAT / TEXT_LOG.DAT), then
  Logo, then the title loop. Everything after that is script.
* One logical frame = 60 Hz fixed (`app+0x1948` timer, 16 ms). Per frame:
  poll input → tick animation tasks → compose → **run the VM until `end`** (1..5
  times if late) → present. Natives never block (except modal OS dialogs);
  waiting is done by scripts looping `poll native → end → jmp`.
* VFS: path `X\Y.EXT` → absolute `<exedir>\x\y.ext`; **loose file on disk wins**,
  then extra search dirs, then archives in mount order (PATCHn > … > PATCH2 >
  PATCH > SCRIPT > SYS > CHR > PIC > BGM > SE > VOICE > DATA). Case-insensitive.
* Saves contain **no VM state** (no pc/stack). They hold the *scope-1* user
  variables (snapshot taken at scene start + a few always-live vars), the
  backlog, a 100×75 thumbnail, a title and a text preview. Loading returns to
  MAIN, which restarts the scene script named in `G[805]`; the scene
  fast-forwards to the saved message number (`G[806]`) replaying recorded
  choices (`G[807]`/`G[808]`). A port only needs to reproduce the variable
  machinery; script code does the resume.
* Three copies of variable memory: **copy0 = live**, **copy1 = pristine initial
  values (for "new game" reset)**, **copy2 = save-point snapshot**.
* Global data: `GAMEDATA.DAT` (scope-2 vars, 757 of them) and `TEXT_LOG.DAT`
  (per-script read-message / choice bitsets + per-character voice settings).
  Both written by MAIN on restart (N_23) and by the engine destructor.
* Windows-only natives (menus, window styles, thread priority, keyboard hooks,
  DVD checks, font enumeration, MessageBox) → stubs; table in §7.

---------------------------------------------------------------------------

## 1. Global singletons and object layout

`DAT_00503248 = app` (set in ctor FUN_004116f0). Sub-object pointers assigned in
FUN_004134c0 (app vtable +0x24) **[V]**:

| global | value | meaning |
|---|---|---|
| DAT_0050323c | app+0x168 | VFS / file manager (vtable 0x4ef110). `DAT_00503f34`, `DAT_00503f60` also point here |
| DAT_00503238 | app+0x194 | sound device [G] |
| DAT_0050324c | app+0x9c0 | display/device object (fonts list at +0x1c4, present code) |
| DAT_00503218 | app+0x19a8 | timer-task list #1 (sound fades etc., ticked every loop) |
| DAT_00503214 | app+0x19b8 | timer-task list #2 (sprite/anim tasks, ticked once per frame) |
| DAT_00503224 | app+0x19cc | transition registry ("cross","scroll","mosaic","rotaxis","trans",…) |
| DAT_00503220 | app+0x19f0 | bg-effect registry (bg_zoom, bg_rot, bg_rast, bg_haze, bg_blur, bg_anime) |
| DAT_00503f40/DAT_00503228 | app+0x1920 | movie player [G] |
| DAT_0050322c | app+0x1908 | ? |
| DAT_0050321c | app+0x1a14 | holds pointer to DAT_00503214 |
| DAT_00503254/50 | *(app+0xbec)/*(app+0xbf0) | DirectInput objects (must be non-null) |
| app+0x1904 | virtual-button map (20 buttons) used by N_220.. |
| app+0x19c8 | script engine |
| DAT_00503258 | eng+0x370 | message log / backlog object |
| DAT_00503fb0 | default text font object (set by N_a6) |

Timer-task list (vtable 0x4edd34, tick = vt+0x30 FUN_0047e4f0(now_ms)) **[V]**:
doubly linked list of task objects `{vt, …, +8 nextTime, +0xc interval, +0x10
repeatCount, +0x14 u8 finished, +0x18 paused}`; for each task with `!paused &&
nextTime<=now`: call `vt+8(now)`; returns 0 = keep (decrement repeat count, if
it hits 0 → finish; `nextTime += interval`, resync if behind); nonzero or
finished → `vt+0(1)` delete and unlink. vt+0x20(-1)/vt+0x28(-1) pause/resume all.

---------------------------------------------------------------------------

## 2. Start-up sequence

`entry` (CRT, 0x4dc5f5) → FUN_00411690 (WinMain): build app on stack
(FUN_004116f0) → FUN_00439680(app, hInst, hPrev, cmdline, nShow) → app dtor.

FUN_00439680 **[V]**: `r = vt+4()` (FUN_0047aa50 init). If `r==0`:
* `app+0x1c == 0` (single-threaded): `vt+8()` = FUN_00401a90 (whole game loop
  in this thread), then `vt+0xc()` exit.
* `app+0x1c != 0` (threaded; `app+0x1c = USER.CFG byte 0xd22`, **=1 in the
  shipped USER.CFG**): the game loop runs in a worker thread (vt+0x34
  FUN_0047d390: vt+0x38 FUN_00401030 init, then `while (thread alive &&
  vt+0x3c()==0)`, vt+0x40 cleanup); the main thread runs a GetMessage loop.
For a port: single-threaded, one loop.

### 2.1 FUN_0047aa50 (app vt+4) — in order **[V]**
1. `vt+0x18` = **FUN_00411f50** (game-specific config, §2.2). Nonzero → abort.
2. If `app+0xd07==1 && app+0xd6a==2` → `app+0xd6a=0` (window mode not
   allowed → ask).
3. If `app+0xd08==0` and the exe drive is a CD-ROM → "ハードディスクから起動して
   ください", exit 2.
4. If `app+0xd06` (=1): single-instance mutex `"Easy Game System DX %s"`
   (title); if it exists, focus the old window and exit 3.
5. Register window-message handlers (WM_* and private 0x9000..0x9009).
6. FUN_00476cf0: desktop must be ≥16 bpp and larger than 800×600 (errors
   "画面モードをHighColor(16bit)以上…", "画面解像度を…").
7. Optional `SYS_LOG.TXT` (app+0xd30): dumps OS/CPU/DirectX info.
8. DirectDraw/DirectInput/DirectSound setup; `vt+0x20` FUN_004132c0: if
   `app+0xd6a==0` or Ctrl held → screen-mode dialog (resource 0x68); window
   class/handlers. Create the window (`app+0x150` = HWND).
9. `vt+0x24` = **FUN_004134c0**: subsystem globals (§1); load `tvpgl.dll`
   (TVP blend library, `DAT_00503874`), if `app+0xe2f` load
   `DS_SPRITE.DLL` (if `0xd27`) or `DS_PRIM.DLL`, and `DS_MOVIE.DLL`;
   default virtual-button bindings (§7.9); sound init; frame timer
   (`FUN_00413e40(-1)`), etc.

Exit (`vt+0xc` FUN_0047cd60): destroy window, `vt+0x28` (FUN_00413a50: fatal
message for `app+0xd79` 1/2, free plugins), restore screensaver/power settings,
release mutex, `vt+0x2c` = **FUN_00413ae0 (write USER.CFG + APP_CFG.DAT)**.
The script-engine destructor (FUN_004223f0) writes GAMEDATA.DAT and TEXT_LOG.DAT.

### 2.2 FUN_00411f50 — files & config **[V]**
1. Register decoders: "ELG format" (image), "Bitmap format", archive
   "LPK format" (id 1, control fn FUN_004876a0), `wuvorbis.dll` "Ogg Vorbis
   format"; plugin dir `plugin`; transitions & bg effects.
2. Command line `/L<dir>` → base dir `DAT_00503e30` (default = exe dir, with
   trailing `\`).
3. Mount `PATCH.LPK` (VFS vt+0x30, args `(name, -2, prefixMode=0,
   append=1)`); only if that succeeded: `PATCH2.LPK`, `PATCH3.LPK`, … (append=0)
   until one fails. Mount `SCRIPT.LPK` `(name, -2, prefixMode=1, append=1)`.
4. `SCRIPT\LL_APP.SYS` → app+0xcf4 (§2.3). Missing → "アプリケーション設定ファ
   イルの読み込みに失敗", abort.
5. `SCRIPT\LL_APP.INF` → app+0xd0b.. (§2.4).
6. Compute config/save directories (`app+0xd09`, §2.5), read `USER.CFG`
   (§2.6), then `<savedir>APP_CFG.DAT` (§2.7).
7. If `app+0xed0` and `app+0xd6a==1` → `app+0xd6a=2`.
8. `SET_UP.IFG` ("IFG1", name, flag) + registry
   `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\<name>`:
   `EgsInstallInfo` byte[1] → **app+0xef0 (install type)**,
   `EgsInstallSrcPath` → app+0xef4 (string). Port: install type 0, no path.
9. If the save folder was newly chosen, write USER.CFG immediately.

### 2.3 LL_APP.SYS (25 bytes) **[V]**
`u16 n (=0x17)`, then n bytes copied verbatim to app+0xcf4:

| off | app | value | meaning |
|---|---|---|---|
| 0 | 0xcf4 | u16 800 | logical screen width |
| 2 | 0xcf6 | u16 600 | logical screen height |
| 4 | 0xcf8 | u32 0 | window style — **overwritten with 0x00CB0000** |
| 8 | 0xcfc | u32 0x200 | ex-style (WS_EX_CLIENTEDGE) [G] |
| 12 | 0xd00 | u16 0 | icon id — overwritten with 0x67 |
| 14 | 0xd02 | u16 0 | cursor resource (if ≠0 → 0x65); also enables fullscreen cursor hiding |
| 16 | 0xd04 | u16 1 | menu present (≠0 → menu resource 0x66) |
| 18 | 0xd06 | 1 | single instance mutex |
| 19 | 0xd07 | 2 | window-mode/resizing capability (0 none, 1, 2 = maximize box) [G] |
| 20 | 0xd08 | 0 | 0 = refuse to run from CD-ROM |
| 21 | 0xd09 | 2 | save-location mode (§2.5) |
| 22 | 0xd0a | 0 | fullscreen-cursor related [G] |

### 2.4 LL_APP.INF (97 bytes) **[V]**
`u8 verMajor(→0xd0b)=1, u8 verMinor(→0xd0c)=0, u8 verSub(→0xd0d)=0`, then five
`u8 len + len bytes` strings (len includes NUL; len 0 → empty / NULL):
app+0xd0e title/window class "俺たちに翼はない", app+0xd12 (same, 2nd name),
app+0xd16 "Copyright (C) 2009 Navel", app+0xd1a "http://www.project-navel.com/",
app+0xd1e save-folder name override (empty → NULL). Version dialog shows
"Version %d.%02d" (+ letter if 0xd0d).

### 2.5 Config and save directories (app+0xd09) **[V]**
Folder name `F` = app+0xd1e, or `title + " SaveData"` (note leading space) →
"俺たちに翼はない SaveData".
* USER.CFG location: mode 0 → `CSIDL_PERSONAL\F\USER.CFG`; 2/3 →
  `CSIDL_APPDATA\F\USER.CFG`; 4 → `CSIDL_COMMON_APPDATA` (fallback APPDATA);
  1 → `<exedir>USER.CFG`. If missing, `<exedir>USER.CFG` (shipped template) is
  copied there.
* Save dir `app+0xeec` (string with trailing `\`; returned by app vt+0x1c):
  mode 1 → `<exedir>SAVEDATA\`; **mode 2 (this game)** → the path string stored
  in USER.CFG (`app+0xe39`); if absent or not existing, the user is asked to pick
  a folder (default My Documents) and `<chosen>\F\` is created and stored back
  into USER.CFG; modes 0/3/4 → same dir as USER.CFG.
* Port: one app-sandbox directory for everything.

### 2.6 USER.CFG (shipped template 111 bytes) **[V]**
Written by FUN_00413ae0, read by FUN_00411f50. Four blocks `u16 len + bytes`:

| block | len | dest | content |
|---|---|---|---|
| A | 0x48 | app+0xd22..0xd69 | system/DirectX settings (from UserConfig.exe) |
| B | 0x0e | app+0xd6a..0xd77 | display settings |
| C | 0x11 | app+0xe28..0xe38 | engine options |
| D | n | `app+0xe39` (char*) | save-folder path (n = strlen+1, 0 = none) |

Known fields (consumers in parentheses):
* A: 0xd22 **threaded game loop** (=1 shipped; →app+0x1c); 0xd23..0xd26 CPU
  feature enables (MMX/SSE… `DAT_00503914/18/1c`); 0xd27 use DS_SPRITE.DLL
  path / vsync mode variant; 0xd29..0xd2c display options; 0xd30 write
  SYS_LOG.TXT; 0xd32 extra mouse-message mode (=2 registers WM_LBUTTON… handlers);
  0xd34..0xd38 sound/display flags; 0xd3a/0xd3b/0xd3c restore screensaver /
  power state; 0xd3d..0xd69 DirectDraw device GUIDs/rects. Rest of block A in the
  shipped file is uninitialised stack garbage — **ignore**.
* B: 0xd6a screen mode (0 = ask with dialog, 1 = fullscreen, 2 = window; shipped
  2), 0xd6b fullscreen bpp, 0xd6c..0xd73 window position (−1 = default; reset
  to −1 when `app+0xed1==0`), 0xd74 move cursor onto default dialog button,
  0xd75 u16 fullscreen cursor auto-hide delay (ms), 0xd77 pause when window
  inactive.
* C (defaults from ctor FUN_004116f0 / shipped file): 0xe28 VM JIT enable (0),
  0xe29 frame-sync mode (0 auto: vsync if refresh is 60 Hz, 1 timer, 2 vsync),
  0xe2a use QueryPerformanceCounter (1), 0xe2b vsync sub-mode, 0xe2c don't
  filter WM_MOUSEMOVE/WM_SETCURSOR/WM_NCHITTEST in the message pump, 0xe2d
  frame-skip enable (→ timer+6, `app+0x194e`), 0xe2e, 0xe2f load DS_*.DLL
  (1), 0xe30 (→`DAT_004fd4b8`), 0xe31 (no reader found), 0xe32 sound buffer
  size code, 0xe33 u16 glyph-cache entries (0x200), 0xe35 allow auto vsync
  toggling (N_f/N_10), 0xe36, 0xe37 disable N_13/N_14, 0xe38 (→`DAT_004fc479`).

### 2.7 APP_CFG.DAT **[V]**
`<savedir>APP_CFG.DAT` = `u32 size (0xa4) + 164 bytes` = raw copy of
**app+0xe40..0xee3**, the game-option block exposed to scripts by N_8/N_9 (§7.2).
On load, `size` bytes are copied (a short file just leaves defaults). Not
present on first boot → constructor defaults.

### 2.8 Script engine creation — FUN_00401a90 / FUN_00401030 → FUN_00414100 **[V]**
1. `app+0x194e = app+0xe2d` (frame skip), display init.
2. `eng = new 0x404; FUN_004154b0(eng)`: base ctor FUN_004221e0 (VM at eng+4,
   stack 0x4000 bytes, natives N_0..N_2, T[] = retTgsExp→eng+0xe0,
   ScriptBootFlg→eng+0xe4, retAppProc→eng+0xe8, all 0), seed the RNG from
   GetTickCount (§7.6 N_52), register natives 3..797.
3. If `app+0xe28` → enable JIT (`FUN_004334a0(vm,1)`). Port: ignore.
4. **FUN_00424440(eng, "SCRIPT\\VOICEINF.DAT")** (runs once, guard eng+0xdc):
   loads SCRIPT\SOB_INIT.DAT, SCRIPT\USER_VAR.SOB, SCRIPT\FUNC_LIB.SOB, then
   VOICEINF.DAT (§5). Error → "グローバルスクリプトのロードに失敗".
5. FUN_0040b8f0: push voice/sound options (app+0xe8d..0xea0) into the voice
   manager `DAT_00503630`.
6. **FUN_00424f80(eng, "MAIN")**. Errors: 1 "メインスクリプトを開けません", 2 "…が不正
   です" (bad magic, or name not in SOB_INIT table), 3 "デバッグモード用のスクリプト
   です" (relocation failure).

### 2.9 What the first scripts do **[V]** (build/dis/main.txt, gameinit.txt)
MAIN:
* `T[1]` (ScriptBootFlg) == 0 (cold boot) → `N_0("GameInit")`, then
  `N_0("Logo")`, then loop: `N_0("Title")` or `N_0("Title2")` (chosen by
  `G[4] & G[5]`), dispatch on `G[910]` (title result): 0/new game → call FUNC_LIB
  L+2224c (reset scope-1 vars from copy1, N_1a(2,0)), `strcpy(G[805],"S00_00")`,
  `N_0("AdvInit")`, `N_0(G[805])` (scene chain); 1 → "Gallery"; 3 → "BgmMode";
  2 or <0 (= a game was loaded) → restart path below.
* `T[1] != 0` or load → `N_23()` (write GAMEDATA/TEXT_LOG), `N_0("GameReset")`,
  then if the local flag > 0 call FUNC_LIB L+22364 (resume setup) and
  `N_0("AdvInit")`, `N_0(G[805])`.

GameInit (order):
1. `N_6(21)` tvpgl loaded? else MessageBox "実行に必要なDLLがロードされていません" + N_2.
2. `N_7(0,buf,260)` + `N_3e(buf,"{793485B9-B82A-4efc-A900-ABCC8D3FA43D}")`
   integrity check (string is hard-coded in the EXE; port: N_7(0) must return it).
3. `N_5(12)` install type; if not 0/3: look for the DVD (`N_7(1)` install
   source + "ORE_TUBA.ID" via `N_62`, else `N_61("ORE_TUBA.ID")` loop with
   `N_313` "ディスクを…入れて下さい"); add `<dvd>\SET_UP` as search dir (`N_5c`).
   **Port: make N_5(12) return 0 → skipped.**
4. For sys, chr, pic, bgm, se, voice, data: `N_66(1,1,key1,key2)` then
   `N_67("x.lpk", -1, 1, 1)` (keys = tools/gameinit_keys.py output).
5. `N_22("GAMEDATA.DAT")`, `N_24()` (load), `N_1e9("TEXT_LOG.DAT")`, `N_1ea()` (load).
6. Fonts (N_a3/N_a4 "ＭＳ Ｐゴシック"/"ＭＳ ゴシック" sizes 14..22), N_a6 default font,
   N_37 thumbnail flags, N_27/N_28/N_29 save-info registration (FUNC_LIB
   L+227ac), etc.

---------------------------------------------------------------------------

## 3. Main loop and timing

### 3.1 Frame timer (app+0x1948, FUN_004388d0/FUN_00438900/FUN_00438a40) **[V]**
Constructed with **fps = 60** (`FUN_004388d0(app+0x1948, 0x3c, 0)`); never
changed by shipped scripts (N_15 = set fps is unused).

| off | meaning |
|---|---|
| +0 | fps |
| +4 | u8 use QPC (`= fps>0 && app+0xe2a && not vsync mode && QPC available`) |
| +5 | u8 vsync-paced (no software throttling) = `app+0xae5 != 0` |
| +6 | u8 frame-skip enabled (`app+0x194e`) |
| +8 | late frames this tick (= elapsed/interval − 1, ≥0) — read as `app+0x1950` |
| +0xc/+0x10 | fps counter (frames last second / running) |
| +0x18 | QPC frequency; +0x20 ticks per frame; +0x28 last frame QPC |
| +0x48 | interval ms = 1000/fps (= 16) |
| +0x4c/+0x50 | last frame time / accumulated ms |
| +0x58 | ms until next frame (`app+0x19a0`, used for Sleep) |

`FUN_00438a40` returns true when a frame is due: GetTickCount path →
`now - last >= 16` (so ~62.5 fps); QPC path → exactly 1/60 s. When due and
frame-skip on, `+8 = elapsed/interval − 1`. In vsync mode (+5) it is always
due and pacing comes from the flip.
`app+0xae5` (vsync mode) = `!(app+0xa30 bit 25) && (0xe29==2 || (0xe29==0 &&
(refresh==60 || app+0xa42=='<'))) ? 0xe2b+1 : 0` (FUN_00413e40).

### 3.2 Per-iteration order (threaded variant FUN_00401080; the single-thread
variant FUN_004016b0 + FUN_00401a90 is equivalent) **[V]**
1. If suspended (`app+0x13a`) → Sleep(50), return.
2. Tick task list #1 (`app+0x19a8`, sound) with `now`.
3. Minimise/deactivate pause state `app+0x1ac0`: 1 → pause all task-list-#2
   tasks, 2 → stay paused (Sleep 50), 3 → resume.
4. Window inactive (`app+0x135==0`) and (`0xd77` pause-when-inactive or
   fullscreen `0xa40==1` or `0x137`) → Sleep(50), return.
5. `app+0xd79 != 0` (fatal) → quit.
6. Frame not due → sleep `app+0x19a0` ms (or keep presenting when
   `DAT_00503240` "busy" counter from N_13 is non-zero) and return.
7. Frame due:
   a. vsync mode → Sleep(3) (or 0 if `app+0x1ab5`).
   b. **Mouse** (FUN_00436220: cursor pos scaled to 800×600, fullscreen
      cursor auto-hide after `0xd75` ms), **keyboard/joystick** poll
      (FUN_00435ea0, double-buffered DirectInput state).
   c. optional vblank wait (`app+0x1ab4`, N_c3), tick task list #2
      (`app+0x19b8`, animations) with `now`, **compose** the layer tree into the
      back buffer (FUN_00436db0 on app+0xab8).
   d. `k = late frames` (`app+0x1950`; >4 → 0); Sleep(remaining).
   e. **Run the VM** FUN_004252d0 once, then `k` more times (each run = until
      the script executes `end`). After each run, handle the interrupt request
      `app+0x1af0` (§5.6).
   f. **Present** (FUN_00438270 windowed blit / FUN_00437ff0 flip).
8. VM return codes: 0 → script engine stopped (N_2) → app exits; 1 → normal;
   2 → "不正なスクリプトを実行しました"; 3 → "Jump命令でスクリプトのロードに失敗";
   4 → "Call命令でスクリプトのロードに失敗"; interrupt failure → "スクリプト割り込みに
   失敗しました". All fatal.

(Single-thread variant FUN_00401a90: runs the VM `max(1,k)` times per frame;
message pump FUN_004397e0 = PeekMessage loop that drops mouse-move-type
messages unless `app+0xe2c`.)

Note the one-frame latency: composition (7c) uses the state *before* this
frame's VM run; a port can do input → VM → tick → compose → present.

### 3.3 Script scheduling & waiting **[V]**
* FUN_004252d0 runs the interpreter until `end` (yield, returns 1) or the top
  level `ret` hits a null return address (returns 0). Mode flag
  `DAT_0050382c` set by N_0/N_1/N_2:
  * N_0 `call` (mode 2): script continues to its `end`; scheduler pushes pc,
    loads `SCRIPT\<name>.SOB` as nested script (depth `DAT_00503724`++),
    **continues running it in the same tick**. When it returns (top `ret` with
    null) the record is freed (FUN_00425110), pc is popped, caller continues
    in the same tick.
  * N_1 `jump` (mode 1): script must follow with `ret` (VM returns 0); the
    current script record is replaced by the new one at the same depth.
  * N_2 (mode 3): stop → FUN_004252d0 returns 0 → application exit.
* There are **no blocking natives**. Waits are written in script, e.g. logo:
  `L: end; r = N_cf(obj) (busy?); if busy { if N_242()||N_227() (click/enter)
  → skip } jmp L`. A native that starts something (fade, sound, movie) returns
  immediately; scripts poll a status native each frame.
* Modal exceptions: N_311/N_313/N_314 and N_18(…,1) block the game thread while
  an OS dialog runs (main thread keeps pumping).

---------------------------------------------------------------------------

## 4. Virtual file system (app+0x168, vtable 0x4ef110) **[V]**

### 4.1 Path normalisation (FUN_004749f0)
* `\\server…` or anything containing `:` → absolute as-is.
* Otherwise relative to base dir `DAT_00503e30` (exe dir or `/L` dir); leading
  `.\` / `..\` handled; `/` → `\`. SJIS-aware (lead bytes 0x81–0x9F, 0xE0–0xFC).
* Before archive lookup the absolute path is **CharLowerA'd** → case-insensitive.

### 4.2 Lookup order for reads (vt+8 FUN_00472f00 "load whole file", vt+4 FUN_00472b10 "open stream")
1. `CreateFile(<base>\path)` — a loose file on disk always wins.
2. If the path was relative: each extra search dir (list at fs+8, added by
   N_5c / vt+0x14 FUN_004732b0, stored lowercased with trailing `\`) for a loose
   file.
3. Archives (FUN_00472e20): mount list at fs+0x24, each mount record
   `{archive object, char* prefix (lowercase absolute dir + "\"), int param2,
   int groupMarker}`. The first mount whose prefix is a prefix of the path gets
   the remainder (e.g. `ef05a.elg`); on miss, the other mounts sharing the
   prefix group are tried in list order. Then step 3 is retried with each search
   dir as base.

Mount (vt+0x30 FUN_00473810, args `(name, param2, prefixMode, append, out)`):
archive file is located like a loose file (base dir, then search dirs).
`prefixMode 0` → prefix = archive's directory (contents carry full relative
paths: PATCH*.LPK); `1` → prefix = archive path without extension + `\`
(`…\script\`, `…\sys\`, `…\pic\`…). `append=0` inserts *before* other mounts
with the same prefix (higher priority), `append=1` after.

Effective priority with shipped data: loose file > search-dir file >
PATCHn (highest n first) > … > PATCH2 > PATCH > SCRIPT.LPK > sys, chr, pic, bgm,
se, voice, data (gameinit order). PATCH2+ are only probed if PATCH.LPK exists
and must be consecutive. Example: `PIC\EF05A.ELG` → `c:\game\pic\ef05a.elg` →
loose → pic.lpk entry `ef05a.elg`. `SCRIPT\MAIN.SOB` → script.lpk entry
`main.sob`.

Archive key handling: N_66(1, 1, k1, k2) calls the LPK format control function
FUN_004876a0 (format id 1): cmd 1 sets `DAT_00503f48=k1, DAT_00503f50=k2`
(used by the next mount); cmd 0 sets `DAT_00503f4c` flag.

### 4.3 File object (0x118+ bytes; FUN_00474fb0 ctor, FUN_00474fe0 dtor)
| fn | purpose |
|---|---|
| FUN_004750c0(obj, path) | load whole file through VFS into a GlobalAlloc buffer; returns 0 ok, 1 no VFS, 3 not found, 8 OOM… |
| FUN_004750a0(obj) | pointer to the loaded bytes (size at obj+0x10c) |
| FUN_004752b0(obj, path) | open stream via VFS (returns 0 on success) |
| FUN_00475490(obj, buf, n) | read |
| FUN_00475390 / FUN_004753e0 | seek absolute / relative |
| FUN_00475660(obj, path, create) | open **disk** file for writing (CREATE_ALWAYS if create) |
| FUN_004756d0(obj, path) | open disk file read/write (existing) |
| FUN_00475760(obj, buf, n) | write |
| FUN_00475ad0(path) | exists? — **loose files only** (vt+0x20 FindFirstFile on base + search dirs) |
| FUN_00475970(path) | size of loose file or −1 |
| FUN_004759c0(path, …) | last-write time → local Y/M/D/weekday/H/M |

Save files are always absolute paths in the save dir, so they bypass archives.

---------------------------------------------------------------------------

## 5. Script loading & the engine object

### 5.1 Engine layout (eng = *(app+0x19c8)) **[V]**
| off | meaning |
|---|---|
| +4 | VM: +8 pc (eng+0xc), +0xc sp (eng+0x10), +0x10 ret, +0x18 JIT flag |
| +0x20..0x2c | vector<ScriptRec*> loaded scripts (begin +0x24, end +0x28); FUNC_LIB is element 0. `ScriptRec{u8* blob, JIT*, int sobInitIndex}` |
| +0x30 | VM stack size 0x4000 |
| +0x38 | native table (vector, begin) |
| +0x48 | T[] table (12-byte entries) |
| +0x54/58/5c | G table copy0 (12-byte `{addr, u8 type, u8 scope, pad, u32 size}`) |
| +0x68 | copy0 string-slot vector; +0x74 copy0 memory (**live**) |
| +0x7c, +0x8c, +0x98 | copy1 G table / slots / memory (**initial values**) |
| +0xa0, +0xb0, +0xbc | copy2 G table / slots / memory (**save snapshot**) |
| +0xc0 | char* GAMEDATA file name (N_22) |
| +0xc8/+0xcc | vector<int> "always save live value" var indices (N_28) |
| +0xd4 | FUNC_LIB blob base (relocation type 3 `L+`) |
| +0xd8 | current script blob |
| +0xdc | u8 globals-loaded guard |
| +0xe0/+0xe4/+0xe8 | T[0] retTgsExp / T[1] ScriptBootFlg / T[2] retAppProc |
| +0xec/+0xf0 | SOB_INIT entry count / table (40-byte entries) |
| +0xf4 | current script's SOB_INIT entry |
| +0xf8 | char* TEXT_LOG file name (N_1e9) |
| +0xfc/+0x100 | voice-group count / table (16-byte entries) |
| +0x104/+0x108 | voice-prefix count / table (8-byte `{char* prefix, int group}`) |
| +0x10c | voice file extension ("OGG") |
| +0x114 | thumbnail source object (N_29) ; +0x118 char* thumbnail image base name |
| +0x11c | u16 interrupt boot code (N_27) |
| +0x120 | int* "include text preview" flag (N_27, → &G[909]) |
| +0x124 | char* save title (N_27, → G[804]) |
| +0x128/+0x12c | in-memory state snapshot blob / size (N_31/N_32) |
| +0x130/+0x134 | int* thumbnail mode flags (N_37) |
| +0x370 | message log / backlog object (DAT_00503258); +0x390 byte non-empty [G] |

### 5.2 FUN_00424f80(eng, name) — load a scene/system script **[V]**
`SCRIPT\<name>.SOB` via VFS; magic must be `SOB0`; if a SOB_INIT table exists the
**uppercased name must be in it** (index → ScriptRec.sobInitIndex, eng+0xf4);
then FUN_00424d10(blob+4, name, index, isLib=0): allocate a copy of the code
blob, push ScriptRec, apply the 5 relocation tables (G: `G[val].addr` of copy0;
N: native ptr; T: T entry addr; L: eng+0xd4+val; @: blob+val), table
terminator must be 0; set VM pc = blob, push a null return address. FUNC_LIB
(`SOB2`) goes through the same function with isLib=1 → eng+0xd4, not executed.

### 5.3 SOB_INIT.DAT (5458 bytes, 412 entries) **[V]**
`u16 count`, then per entry `u8 len, len bytes name (UPPERCASE, NUL-padded),
u16 nMessages, u16 nChoices`. Entry in memory (40 bytes): `+0 char* name,
+4 u8 len, +5 u8 tmp flag, +8 bitset A {+4 nbits, +8 nbytes, +0xc data},
+0x18 bitset B {same}`. Bitset A = per-message "already read" flags (count =
number of message records in the scene, e.g. S04_27A: 157), B = per-option
"already chosen" flags (2–3 for the 24 scenes ending in a choice). First entry is
MAIN, last is FUNC_LIB. Persisted in TEXT_LOG.DAT. Accessors FUN_00423c90 (get
A bit), FUN_00423ce0 (set A), FUN_00423d30/FUN_00423d80 (B); `param_2!=0`
selects the last table entry instead of the current script (used by other
natives, see text/message docs).

### 5.4 USER_VAR.SOB (`SOB1`, 15748 bytes) **[V]**
```
"SOB1"
u32 memSize (6424) ; memSize bytes = initial variable memory
u32 nVars (916)    ; nVars × { u8 type; u8 scope; u32 offset; u32 size }   (10 bytes, packed)
u32 nFix (18)      ; nFix × { u32 off; u32 target }   → *(u32*)(mem+off) = mem+target
u32 0              ; terminator (≠0 → load error 4)
```
* The memory block is copied **three times** (eng+0x74, +0x98, +0xbc); fix-ups
  are applied to each copy relative to its own base.
* `type 0` = plain value (`G[i].addr = mem+offset`), `type 1` = string buffer:
  a 4-byte slot holding `mem+offset` is appended to the slot vector and
  `G[i].addr = &slot` (so `G[i]` evaluates to a `char*`), `type 2` = scratch /
  pointer value (never saved). The three G tables differ only in the copy they
  point to; relocations in scripts use copy0.
* `scope`: **0 = volatile** (106 vars), **1 = per-save game state** (53 vars,
  e.g. G[804] save title, G[805] current scene name, G[806] message counter),
  **2 = global/system** (757 vars, saved in GAMEDATA.DAT).
* Shipped counts: (type,scope) (0,2)=757, (0,0)=79, (0,1)=47, (1,0)=20,
  (2,0)=7, (1,1)=6.

Why three copies **[V]**:
* copy0: what scripts read/write.
* copy1: never written after load → initial values. N_1a (FUN_00422b10)
  copies copy1→copy0 for a scope/type filter (new game: `N_1a(2,0)`).
* copy2: save snapshot. N_1c (FUN_00422cb0) copies copy0→copy2 (each scene
  script starts with FUNC_LIB L+222cc → `N_1c(2,0)`); FUN_00423070 copies the
  N_28-registered vars copy0→copy2 right before a save; FUN_004230e0 single
  var; FUN_00422d90 copy2→copy0 (unused by my natives).
* Filter arguments: `scopeSel` 1 → scope 0, 2 → scope 1, 3 → scope 2, other →
  all; `typeSel` 0 → all (type 1 copies the pointed-to buffer), 1 → type 0
  only, 2 → type 1 only, 3 → type 2 only. Copies use `size` bytes.

### 5.5 VOICEINF.DAT (1039 bytes) **[V]**
```
u8 len, len bytes ext ("OGG\0")        → eng+0x10c
u32 nPrefixes (99), u32 nGroups (34)
nGroups × { u8 len, name (cp932, NUL-padded);
            u8 flagLen, flagLen bytes;          flagLen>0 → hidden=1 (+0xd); flagLen>1 → alt name (+4)
            u32 k, k × { u8 len, prefix } }      prefixes → table {prefix, group}
```
Group entry (16 B): `+0 name, +4 alt name, +8 int volume (default 0x100 [G]),
+0xc u8 voice on, +0xd u8 name hidden`. Hidden groups display "？？？" (N_b
field 5) until a script clears the flag. Groups: 渡来 明日香 ASU, 山科 京 MIY, …,
その他・女 (SAW, X01..X27), その他・男 (DAI, DAB, BOK, Y01..Y35). Persisted in
TEXT_LOG.DAT.

### 5.6 Interrupt / restart (FUN_0041d6e0, FUN_004251d0) **[V]**
When `app+0x1af0 ∈ {1,2}` after a VM run: `FUN_004251d0(eng, ±eng.u16[0x11c])`
(sign by 0x1af0) = reset VM stack, set **T[1] ScriptBootFlg**, depth=0, free
all script records except FUNC_LIB; then `FUN_00424f80("MAIN")`; release the
"auto/skip" nesting counters. **No code in the EXE sets app+0x1af0** (only
clears) — the mechanism is dormant in this game; loading uses the script path
in §6.5. MAIN still checks T[1].

---------------------------------------------------------------------------

## 6. Save / load

### 6.1 Save file `<savedir>DATA%02d%02d.SAV` (bank, slot) — FUN_0041dbe0 (N_2a) **[V]**
```
0x0000  "OT01"
0x0004  u32 0                      version (load fails with 5 if ≠0)
0x0008  thumbnail 100×75, 75 rows × 300 bytes (3 bytes/pixel = low 3 bytes of
        the 32-bpp surface pixel, i.e. B,G,R; row order = surface order, top-down [G])
0x57EC  char[0x2c] title           (lstrcpyn of *(eng+0x124), ≤40 chars)
0x5818  char[0x90] text preview    ("【name】\n" + message, ≤140 bytes, SJIS-safe cut)
0x58A0  u32 storedSize
0x58A4  u32 origSize               (0 = stored raw)
0x58A8  u8  checksum               (8-bit sum of the stored bytes)
0x58A9  stored bytes               (LZSS of the state blob if that is smaller, else raw)
```
LZSS (FUN_00495420 / FUN_00495710): Okumura-style, 4096-byte ring
**zero-filled**, write pos starts 0xFEE, flag byte LSB first, bit 1 = literal
byte, bit 0 = 2-byte match `pos = b0 | (b1 & 0xF0) << 4`, `len = (b1 & 0x0F) + 3`.
Encoder gives up (stores raw) if output ≥ input − 8.

State blob (FUN_0041d970(eng, …, withLog=1)):
```
u8 scopeSel = 0                         (= scope-1 vars)
for each G var with scope==1 and type<2, in table order:
    u8 type ; size bytes  (type 1 = string buffer contents)   — values from copy2
u8 hasLog = withLog && app+0xe5b
if hasLog:  backlog object serialisation (eng+0x370 vt+0x10)
            u32 n ; n bytes  (nested snapshot eng+0x128, n=0 if none)
```
Before serialising, FUN_00423070 refreshes copy2 with the live values of the
N_28-registered vars (G[806], G[807], G[785], G[786], …). Everything else is
"as of the start of the current scene".

Thumbnail source (§7.3 N_29/N_37) **[V/G]**: if `eng+0x118` names an image, the
file `SYS\_<name>` (pre-rendered 100×75 thumbnails `_asu16c.elg`… exist in
SYS.LPK) is used; otherwise the source object (eng+0x114) is rendered and
stretched to 100×75; if `*(eng+0x134)` the thumbnail is blank.
Text preview: last backlog entry, only if `app+0xedd` (option) and log
non-empty and `*(eng+0x120)` (G[909]) non-zero.

Bank/slot conventions **[V]**: `slot < 0` with bank 10 → ring of 10 slots
(next index `app+0xedb`, 0..9; last saved → `app+0xedf`) = **auto-save**;
bank 11 → ring 5..9 (`app+0xedc`; last → `app+0xee0`) = **quick save**.
After any save `app+0xede = 1`. For loading with slot<0 the ring is walked
backwards from the next index to the newest existing file. The script save menu
uses `G[8]`/`G[10]` as bank (page) and a local slot.

Date shown for a save = file last-write time, formatted
`"%04d/%02d/%02d(%s) %02d:%02d"` with weekday from "日月火水木金土".

### 6.2 Load (N_2b → FUN_0041e3f0 → FUN_0041db10) **[V]**
Check magic/version, seek 0x58A0, read header, verify checksum, LZSS-decode,
then: write scope-1 vars into **copy0 (live)** (type byte must match the
table, else error); FUN_00432330 (clear message window/backlog state);
free eng+0x128; if `hasLog` restore the backlog (honouring app+0xe5b) and the
nested snapshot into eng+0x128/+0x12c. **No VM state is restored.**
Return: 0 ok, −1 file missing, 1 open fail, 2 OOM, 3 bad magic/checksum/LZSS,
4 variable mismatch, 5 bad version.

### 6.3 GAMEDATA.DAT (global variables) — FUN_004232d0 write, FUN_00423850 read **[V]**
Path `<savedir>` + `*(eng+0xc0)`. Written with `scopeSel=1` (scope-2 vars) from
**copy0**:
```
u32 storedSize, u32 origSize (0 = raw)
u32 extASize, extA bytes        (optional extra type-0 var; 0 here)
u32 extBSize, extB bytes        (optional extra string var; 0 here)
u8  scopeSel (1), u8 checksum
stored bytes → { u8 type; size bytes } per matching var (same as 6.1, no leading scopeSel)
```
Reader FUN_00423850 checks checksum, decodes, writes into copy0, aborts on type
mismatch (return 2). Written by N_23 and by the engine destructor.

### 6.4 TEXT_LOG.DAT (read flags + voice settings) — FUN_00423e30 write, FUN_00424180 read **[V]**
```
"TLOG"
u32 nGroups ; nGroups × { u32 volume(+8); u8 voiceOn(+0xc); u8 hidden(+0xd) }
u32 storedSize
if storedSize: u32 origSize (0 = raw), u32 nScripts, stored bytes →
   nScripts × { u8 len; name[len]; u32 nbitsA; u32 nbytesA; A bytes; u32 nbitsB; u32 nbytesB; B bytes }
```
Reader: voice table applied only if nGroups matches; per script, bits applied
only when name and bit counts match the current SOB_INIT table.

### 6.5 How a load actually resumes **[V for the code path, G for the fine detail]**
saveload.sob: `N_2b(bank, slot)`; on 0 → `G[855]=1, G[910]=2`, return to the
caller (title or in-game menu → returns up to MAIN). MAIN sees `G[910]==2`
→ N_23, GameReset, FUNC_LIB L+22364 (sets `G[905]=1` replay mode,
`G[906]=G[806]` target message, `G[907]=0` choice-replay index, …), AdvInit,
`N_0(G[805])`. The scene restarts at its beginning; FUNC_LIB's message routine
increments `G[806]` per message and skips until the target, and the choice
routine (L+225cc) replays results from the array `G[807]` (count `G[808]`)
while `G[907] < G[808]`. Port requirement: a fast/skip path for messages and
effects while `G[905]` is set (see message/text docs).

### 6.6 In-memory snapshot (N_31/N_32/N_33/N_34) **[V]**
N_31 serialises the same blob as a save (backlog included unless `app+0xed9`)
into eng+0x128; N_32 restores it (like a load, then frees it); N_33 tests; N_34
frees. Used by FUNC_LIB for "前の選択肢に戻る" (back to previous choice);
the blob also travels inside save files (§6.1).

---------------------------------------------------------------------------

## 7. Natives

Numbering as in `00_vm.md` (`N_k` = table index, hex). Only natives used by
shipped scripts are listed. "Port" column/notes = recommended behaviour.

### 7.1 Window / environment flags (N_3–N_7, N_d, N_13, N_14, N_17–N_19)

**N_3 @0x417da0** `(mode, value)` → 0. Windows-only.
* mode 0: enable/disable the fullscreen toggle (menu item 0x9c41, WS_MAXIMIZEBOX)
  if `app+0xd07`; sets/clears `app+0xd78` bit 2.
* mode 1: `app+0xd78 & 3 = value`; value≠0 → install low-level keyboard hook
  (blocks Win/Alt-Tab in fullscreen) and enables cursor hiding modes; 0 → remove.
  Gallery uses (1,2) and (1,1). **Port: no-op.**

**N_4 @0x417f30** `(index, value)` → 0 — set app flag; **N_5 @0x418190**
`(index)` → value. Index map:

| idx | field | meaning | port |
|---|---|---|---|
| 0 | u8 0xd74 | snap cursor to default dialog button | no-op/store |
| 1 | u8 0xe2d (+0x194e) | frame-skip enable | store, optional |
| 2 | u8 0xd77 | pause when inactive | store; pause on background anyway |
| 3 | u16 0xd75 | fullscreen cursor hide delay ms | store |
| 4 | u8 0xe2f | load DS_*.DLL plugins | store |
| 5 | u8 0xe31 | ? (no reader) | store |
| 6 | u16 0xe33 + DAT_004fc500 | glyph cache size | store |
| 7 | u8 0xee4 | enable "settings" menu items 0x9c45/46 | store |
| 8,9,10 | u8 0xee5..0xee7 | script flags (no engine reader found) | store |
| 11 | u8 0xee8 | window maximize box (if 0xd07) | store |
| 12 (get only) | u8 0xef0 | install type | **return 0** |
| 13 (get only) | u8 0xe36 | ? | store |

**N_6 @0x418290** `(index)` → system info (read-only):
0 `!(app+0xa30 bit25)` (display can vsync [G]); 1 `0x9fc`; 2 vsync mode active
(`0xae5!=0`); 3 `0xae5==2 && fullscreen`; 4 `0xd32` mouse mode; 5 QPC timer in
use (`0x194c`); 6 joystick present; 7/8 CPU feature bytes; **9 `0xd6a-1`
(0 = fullscreen, 1 = window)**; 10 `0xd6b` fullscreen bpp; 11 `0xcd6`; 12
`0xcd4`; 13 `0xcd8`; **14 `0x9ff` supported fullscreen depths bitmask (bit2
16 bpp, bit1 24, bit0 32)**; 15 `0xa34`; 16 video memory; 17 `0xa4c!=0`;
18 `0xa98`; 19 `0xa90`; 20 `0xa94`; **21 tvpgl.dll loaded**; 22 `0xa43`.
Port: 21 → 1, 9 → 1 (window) or 0 per design, 14 → 7, others 0.

**N_7 @0x418420** `(which, buf, maxLen)` → string query. which 0 =
"{793485B9-B82A-4efc-A900-ABCC8D3FA43D}" (integrity check in GameInit), 1 =
install source path (app+0xef4, may be NULL), 2 = save dir (app vt+0x1c), 3 =
NULL. `maxLen==0` → returns strlen+1 (0 for NULL); else copies (lstrcpyn if
too long, empty string if NULL) and returns 0; bad `which` → −1.

**N_d @0x419070** `(mode)` → change display mode. Requires `app+0xd07` and
`app+0xd78 & 4`. mode 3 = windowed (needs desktop >8 bpp and window fits);
0/1/2 = fullscreen at 16/24/32 bpp (needs bit 2/1/0 of `0x9ff`). Sends window
message 0x9002; returns 0 ok, 1 not allowed, 2 unsupported, 3 failed (sets
fatal `app+0xd79=2`). Used by config1 (3 and 0). **Port: return 0, ignore**
(or map to fullscreen toggle on macOS).

**N_13 @0x414000** `()` → enter "busy" mode: `DAT_00503240++`; first entry sets
game-thread priority to BELOW_NORMAL (returns 0; 1 if already busy; 2 no thread;
−1 if `0xe37`). While busy the loop keeps composing/presenting instead of
sleeping. **N_14 @0x414090** `()` → leave: decrement, restore priority at 0,
returns previous count. Used by gallery. **Port: counter only.**

**N_17 @0x419270** `(b)` → `app+0x194e = b&1` (frame-skip on/off at runtime;
not persisted). 0. Port: store.

**N_18 @0x419290** `(cmd, sync)` → 1 (0 if not applicable). Posts (sync=0) or
sends (sync=1) a window command: 0 → WM_COMMAND 0x9c45 "environment settings"
dialog (only if `0xee4`); 3 → WM_CLOSE (quit with "終了しますか" Yes/No);
4 → 0x9c41 screen-mode dialog (only if `0xd78&4`); 5 → 0x9c44 version/about
dialog (resource 0x69). Title uses (5,0), config1 (0,1). **Port: 3 → quit
request with confirm; others no-op.**

**N_19 @0x419360** `()` → `app+0x1af1` = which modal OS dialog is open (1
settings, 4 quit confirm, 5 screen mode, 6 version; 0 none). FUNC_LIB checks it
before acting on input. **Port: 0 (or nonzero while a native alert is up).**

### 7.2 Option block access (N_8, N_9) and voice table (N_a, N_b), N_c

**N_8 @0x4184c0** `(group, index, value)` → 0 (−1 unknown); **N_9 @0x4189e0**
`(group, index)` → value (byte fields zero-extended; group 2/0 returns the
*address* of the 32-byte font-name buffer). Fields live in app+0xe40..0xee3
(persisted in APP_CFG.DAT). Defaults are from the app ctor. Consumers shown
where found; names are guesses unless marked.

| g/i | offset | default | consumer / meaning |
|---|---|---|---|
| 0/0 | e43 | 0 | |
| 0/1 | e44 | 1 | rich-text font flag (FUN_0041d770) |
| 0/2 | e45 | 1 | message skip logic (FUN_00409570) |
| 0/3..6 | e46..e49 | 0,1,0,1 | e48: FUN_004326a0 |
| 0/7 | — | — | always returns 1 on set |
| 0/8..11 | e4a..e4d | 0,0,0,1 | e4c: auto-save enable [G] (checked before N_2a(10,-1)) |
| 0/12 | e50 (u32) | 750 | timing ms (FUN_0041d940) [G auto-mode wait] |
| 0/13 | e54 | 1 | |
| 1/0..6 | e55..e5b | 0,30,1,30,0,50,1 | e55–e59 auto/skip settings (FUN_00409570); **e5a backlog length (n+1 entries)**; **e5b store backlog in saves** |
| 1/7 | e5c (u32) | 1000 | |
| 2/0 | e60 char[32] | "ＭＳ ゴシック" | message font face (set copies ≤32 bytes) |
| 2/1..7 | e80..e86 | 1,0,2,0,0,1,0x8c | e80 antialias, e81 gradient, e82 effect type, e83 bold [G]; e85/e86 text-layer params |
| 2/8 | e88 (u32) | 0 | text-layer param |
| 3/0..4 | e8c..e90 | 0,1,1,1,1 | sound/voice switches (FUN_0040b8f0) |
| 3/5..8 | e94,e98,e9c,ea0 (u32) | 150,256,256,256 | volumes [G] |
| 3/9..14 | ea4..ea9 | 1,0,0,0,0,0x41 | ea9 key code (FUN_00431da0) |
| 3/15 | e91 | 0 | |
| 4/0,1 | eb8, eb9 | 1,0 | key-config flags |
| 4/2..15 | eaa..eb7 | 2c,2d,1e,41,42,43,44,39,32,2e,d2,1f,0e,14 | DirectInput key bindings (DIK codes) |
| 5/0..5 | eba, ec9..ecd | 0,1,0,0,0,1 | joystick config |
| 5/6..19 | ebb..ec8 | 0 | joystick button map |
| 6/0..4 | ece..ed2 | 1,1,0,1,1 | ed0 force window mode; ed1 keep window position |
| 6/5 | ed4 (u32) | 256 | |
| 6/6..8 | ed8..eda | 1,0,1 | **ed9 exclude backlog from N_31 snapshot** |
| 7/0 | ede | 0 | "has saved" (set by N_2a) |
| 8/0..3 | edf..ee2 | 0,0,0,1 | **edf last auto-save slot, ee0 last quick-save slot** |
| (no API) | edb, edc | 0, 5 | auto/quick-save ring indices; edd preview text in saves |

**N_a @0x418f10** `(group, field, value)` → 0 / −1: field 0 → +8 volume
(int), 1 → +0xc voice on, 2 → +0xd hidden. **N_b @0x418f70** `(group,
field)`: group <0 or ≥ count → returns count; field 0/1/2 as above, 3 name,
4 alt name, 5 display name (hidden → alt or "？？？", else name), 6 first voice
prefix of the group. Persisted in TEXT_LOG.DAT. Config2 uses these for the
per-character voice page.

**N_c @0x419040** `(onlyIfDirty, clearDirty, textLayer, textLayer2, font,
applyFont, applyLog)` → previous `app+0x1ac9` ("options changed" flag, set by
the OS settings dialogs). If `onlyIfDirty` and not dirty → returns 0 without
doing anything. Applies option fields to objects: layer type 5/0x6f → vt+0x84
(e85,e88,e86); type 6 → vt+0x88 (…) and +0x40 = e44; `applyFont` → font (or
default `DAT_00503fb0`): face e60, effect e82, gradient e81, antialias e80,
bold e83, recreate if changed; `applyLog` → backlog length e5a. Port:
implement as "re-apply message-window settings".

### 7.3 Save system (N_1a, N_1c, N_22–N_24, N_27–N_34, N_37, N_1e9, N_1ea)

| native | addr | args → ret | behaviour |
|---|---|---|---|
| N_1a | 0x419380 | (scopeSel, typeSel) → 0 | copy1→copy0 (reset vars to initial). `N_1a(2,0)` = new game |
| N_1c | 0x4193c0 | (scopeSel, typeSel) → 0 | copy0→copy2 (save-point snapshot). `N_1c(2,0)` at every scene start |
| N_22 | 0x419490 | (name) → 0 | eng+0xc0 = strdup(name) ("GAMEDATA.DAT") |
| N_23 | 0x4194b0 | () → status | write GAMEDATA (scope-2, live) then TEXT_LOG; returns 2 if no name, else first error / 0 |
| N_24 | 0x4194c0 | () → status | read GAMEDATA (eng+0xc0); 0 ok, 1 missing, 2 corrupt |
| N_1e9 | 0x432370 | (name) → 0 | eng+0xf8 = strdup(name) ("TEXT_LOG.DAT") |
| N_1ea | 0x432390 | () → status | read TEXT_LOG; 0 ok, 1 missing, 2 corrupt |
| N_27 | 0x41eda0 | (code, titlePtr, previewFlagPtr) → 1 / 0 | if code≠0: eng+0x11c=code, +0x124=title (char*), +0x120=int*, **clears the N_28 list**. FUNC_LIB: `N_27(1, G[804], &G[909])` |
| N_28 | 0x41ee10 | (varAddr) → 1 / 0 | append index of the var whose copy0 address == varAddr to eng+0xc8 list (saved with live value). Registered: G[806], G[807], G[785], G[786], … (34 calls) |
| N_29 | 0x41ee40 | (obj, imageName) → 1 | eng+0x114 = thumbnail source object, eng+0x118 = char* (`G[757]`, current BG name → `SYS\_<name>`) |
| N_37 | 0x41f150 | (0, sel, intPtr) → 0 | sel 0 → eng+0x130, 1 → eng+0x134 (thumbnail mode flags) |
| N_2a | 0x41ee70 | (bank, slot) → 0 ok / err | save game (§6.1). slot<0 with bank 10/11 → rings |
| N_2b | 0x41ee90 | (bank, slot) → 0 ok / err | load game (§6.2) |
| N_2c | 0x41eeb0 | (buf, bank, slot, sepMode) → 0 ok, −1 no file, 1 null buf, 2 open err | buf = "『" + title + "』" + sep + "【記録日時 YYYY/MM/DD(曜) HH:MM】"; sep: 0 " ", 1 "\n", else "\r\n". Used for confirm prompts ("…をロードしますか？") |
| N_2d | 0x41eee0 | (bank, slot, imgObj, title44, msgBuf, dateBuf, reflow) → 0/−1/1/2 | read slot info: thumbnail → imgObj (BGR → 32-bit, vt+0x18 bits, vt+0x10 pitch), title (0x2c bytes), preview (0x90 bytes; reflow≠0 → wrap at 34 bytes/line, max 4 lines), date string. Any pointer may be 0 |
| N_2e | 0x41ef10 | (bank, slot, text) → 0/−1/1 | overwrite the preview text at 0x5818 (≤140 bytes, SJIS-safe) |
| N_2f | 0x41ef30 | (srcBank, srcSlot, dstBank, dstSlot) → 0 ok, 1 no src, 3 fail | copy save file |
| N_30 | 0x41ef60 | (bank, slot) → 0 ok, 1 missing, 2 fail | delete save file |
| N_31 | 0x41ef80 | () → – | take in-memory snapshot (eng+0x128) |
| N_32 | 0x41ef90 | () → result of restore, −1 none | restore snapshot (like load) and free it |
| N_33 | 0x41efa0 | () → bool | snapshot exists |
| N_34 | 0x41efc0 | () → 0 | free snapshot |

Note: N_2a's actual result is produced by an exception handler (C++ catch in
FUN_0041dbe0); scripts treat 0 as success ("クイックセーブに失敗しました" otherwise).

### 7.4 Misc script helpers (N_35, N_39–N_4e, N_52)

* **N_35 @0x41efe0** `(obj, mode(0|1), n, strA, strB)` → task ptr or 0.
  Builds a 0x70-byte path-animation task (vtable 0x4efbd0, FUN_00496e70) with
  keyframes from the object's vt+0x24/+0x28 values and `atof(strA/strB)`,
  registers it on task list #2 for `obj`. Belongs to the animation doc;
  **semantics only partially decoded [G]**.
* **N_39 @0x419510** `(dst, src)` → dst: lstrcpyA. 451 uses (scene names into G[805]).
* **N_3a @0x419530** `(dst, src, offset, maxLen)` → 0: copy ≤maxLen bytes from
  `src+offset`, stop at NUL, terminate (byte-based, not SJIS-aware).
* **N_3c @0x4195a0** `(dst, src)` → dst: lstrcatA.
* **N_3e @0x4195d0** `(a, b)` → lstrcmpA result (locale-aware; port: strcmp is
  fine for these ASCII/SJIS literals). **N_3f @0x4195f0** → lstrcmpiA.
* **N_41 @0x419640** `(dst, fmt, args…)` → 0: wvsprintfA with `&args[2]` as
  va_list (`%d %s %02d` …; ints and char* only). Port: implement a mini
  wsprintf over 32-bit words.
* **N_43 @0x419680** `(str)` → float bits of atof(str) (stored as raw IEEE754
  in the result word; 0 if NULL).
* **N_44 @0x4196a0** `(str)` → 0: CharUpperA in place (ASCII only in practice).
* **N_4c @0x419ae0** `(dst, value, count)` → 1: fill `count` dwords (0 if dst NULL).
* **N_4e @0x419b60** `(dst, src, n)` → 1: byte copy (0 if NULL).
* **N_52 @0x419c50** `(n)` → random in [0,n) (0 if n=0). Knuth subtractive
  generator (ran3: 55-entry table, MBIG 1e9) seeded with GetTickCount at engine
  creation. Port: any PRNG.

### 7.5 File system (N_5c, N_61, N_62, N_66, N_67)

* **N_5c @0x419f90** `(dir, append)` → –: VFS add search dir (lowercased,
  trailing `\`), append≠0 → end of list else front. Used for `<DVD>\SET_UP`.
  Port: no-op.
* **N_61 @0x41a020** `(fileName, outRoot)` → 0 if some CD/DVD drive root has
  `fileName` (outRoot = "E:\"), 1 no optical drive, 2 not found. Windows-only;
  port: return 0 with outRoot = data dir (never reached if N_5(12)=0).
* **N_62 @0x41a040** `(path, int* outSize)` → 1 if a loose file exists (size
  stored), else 0. Archives are not searched.
* **N_66 @0x41a0d0** `(formatId, cmd, k1, k2)` → result of the format plugin's
  control function (−2 unknown id). LPK (id 1): cmd 1 sets the next mount's
  keys, cmd 0 sets a flag.
* **N_67 @0x41a100** `(name, param2, prefixMode, append)` → –: mount archive
  (§4.2). Shipped: `(x.lpk, -1, 1, 1)`. Port: open the archive with the keys
  set by the preceding N_66 (or a static key table) and add it to the VFS.

### 7.6 Fonts & text measurement (N_a3–N_bf)
Font objects are owned by the display object's list (app+0x9c0+0x1c4).

| native | addr | args → ret | behaviour |
|---|---|---|---|
| N_a3 | 0x425a70 | (size, face) → font* | create simple GDI font (class 0x4ee600, 100 B); used once ("ＭＳ Ｐゴシック",16) |
| N_a4 | 0x425a90 | (size, face) → font* | create rich glyph-cache font (class 0x4ee580, 0x20b8 B, cache = app+0xe33) |
| N_a5 | 0x425ab0 | (font**) → – | destroy, `*p = 0` |
| N_a6 | 0x425ae0 | (font) → 1 / 0 | `DAT_00503fb0` = default message font |
| N_a7 | 0x425b00 | (font, face) → vt+0x14 | set face (recreate if changed) |
| N_a8 | 0x425b20 | (font, px) → vt+0x18 | set height |
| N_a9 | 0x425b40 | (font, bold, italic) → vt+0x1c | weight 700/400, italic |
| N_ab | 0x425b80 | (font, b) → 1 | vt+0x24: +0x48 antialias [G] |
| N_ac | 0x425ba0 | (font, r, g, b) → 1 | text colour +0x49..0x4b |
| N_ad | 0x425bd0 | (font, effect, dx, dy) → 1 | +0x4c effect (1 shadow with offset dx,dy; 2 outline +2px) |
| N_ae | 0x425c00 | (font, r, g, b) → 1 | effect colour +0x4d..0x4f |
| N_b0 | 0x425c90 | (font, n) → 1 | +0x58 line spacing |
| N_b5 | 0x425db0 | (font, on, rgbTop, rgbBottom) → int | rich font (type 1) only: gradient fill (FUN_0044b410) |
| N_b7 | 0x425e20 | (font, text, &w, &h, &offX, &offY) → lines | measure text (vt+0x28; includes effect extents) |
| N_bc | 0x4260f0 | (obj, x, y, text, font) → 0 ok / err | render text onto a graphic object's surface (obj types 4,7,8,9,10), default font if 0 |
| N_bd | 0x426170 | (charset, flags) → count | enumerate system fonts (EnumFontFamiliesEx) into app+0x1a58; config1 uses (128 SHIFTJIS, 13) |
| N_bf | 0x426260 | (i) → char* | face name of enumerated font i (60-byte entries, name at +0x1c), 0 if out of range |

Port: N_bd/N_bf → expose the bundled font list (1–2 entries); fonts map to
bundled Japanese fonts. (Details of rendering belong to the text doc.)

### 7.7 Input virtual buttons (N_220, N_222, N_227, N_228, N_22f–N_232)
`app+0x1904` = 20 virtual buttons, each a list of `(device, code)` bindings;
device 0 = keyboard (DIK codes), 1 = mouse, ≥2 = joystick n−2. Keyboard state
is double-buffered: "held" = current frame bit 0x80, "triggered" = held now and
not in the previous frame. Default bindings (FUN_004134c0) **[V]**: 0 Up
(DIK 0xC8, joy 0), 1 Down (0xD0, joy 1), 2 Left (0xCB, joy 2), 3 Right (0xCD,
joy 3), 4 Enter (0x1C, numpad 0x9C), 5 Escape (0x01), 18 PageUp (0xC9),
19 PageDown (0xD1); more from the key-config block (FUN_0040b9c0/0040bb90).

| native | addr | args → ret | meaning |
|---|---|---|---|
| N_220 | 0x41ac90 | (btn) → 0/1 | button triggered this frame |
| N_222 | 0x41acd0 | (first, count) → idx / −1 | first triggered button in [first, first+count) (count 0 = to end) |
| N_227 | 0x41ad80 | () → 0/1 | button 4 (decide/Enter) triggered |
| N_228 | 0x41ada0 | () → 0/1 | button 5 (cancel/Esc) triggered |
| N_22d | 0x41ae40 | () → 0/1 | button 4 (decide/Enter) held |
| N_22e | 0x41ae60 | () → 0/1 | button 5 (cancel/Esc) held |
| N_22f..N_232 | 0x41ae80.. | () → 0/1 | buttons 0..3 (Up/Down/Left/Right) **held** |

Port: map touch/gamepad/keyboard to these 20 buttons; mouse clicks are separate
natives (N_242/N_243… — input doc).

### 7.8 Dialogs and window hooks (N_2c7, N_311, N_313, N_314)
* **N_2c7 @0x420b70** `(on)` → 0: on → subclass the main window for text entry
  (FUN_0044aa70 with HWND; sends 0x9007) — used by saveload around editing the
  save comment (then N_2e). off → restore. **Port: show/hide a native text
  field (or a fixed comment).**
* **N_311 @0x41b340** `(p0, text, style)` → 0: synchronous message box
  (window msg 0x9001 with struct `{kind=0, text, p0, (u8)style}`). Scripts pass
  `(0, msg, 4)`; used for fatal messages (then N_2). Port: alert.
* **N_313 @0x41b390** `(p0, text, style)` → u8 result: same with kind 2
  (question box). GameInit DVD prompt `(0, msg, 2)`; nonzero = retry/yes.
  Port: alert returning 1/0.
* **N_314 @0x41b3c0** `(yesNo, x, text)` → 0/1: modal dialog via msg 0x9009
  → `FUN_0040f400(app, text, yesNo?2:0, 0, x, 1)` → Win32 dialog resource
  0x6a (OK) or 0x6c (Yes/No). Used for "セーブ
  データのロードに失敗しました", "スロット%sにセーブしました" etc. Port: in-game or
  native alert.

### 7.9 Windows-only summary
No-op on mobile: N_3, N_4 idx 0/7/11 effects, N_d, N_13/N_14 (counter only),
N_18 (except quit), N_19 (=0), N_5c, N_61 (succeed), N_bd/N_bf (bundled list),
N_2c7 (text field), message boxes (N_311/N_313/N_314 → native alerts),
N_6 hardware queries (constants), N_7(1) (NULL). Keep N_7(0) returning the GUID.

---------------------------------------------------------------------------

## 8. Port checklist (lifecycle)

1. Data dir = folder with `*.LPK` (+ optional loose overrides). Mount
   PATCH*/SCRIPT at boot; let GameInit mount the rest via N_66/N_67.
2. Parse LL_APP.SYS (800×600), LL_APP.INF (title/version); ignore USER.CFG
   except to keep defaults; persist APP_CFG.DAT (164-byte block) in the sandbox.
3. Engine: load SOB_INIT, USER_VAR (3 copies + fixups), FUNC_LIB, VOICEINF;
   T[1]=0; run MAIN.
4. 60 Hz fixed-step loop; VM step until `end`; catch-up ≤4 steps.
5. Saves: implement §6.1–6.4 byte-exact if Windows save import is wanted;
   otherwise any format that stores scope-1 vars (with N_28 live overrides),
   backlog, thumbnail, title, preview, timestamp, nested snapshot.
6. GAMEDATA/TEXT_LOG: write on N_23, on app background/terminate, and at exit.

---------------------------------------------------------------------------

## 9. Open questions

* `app+0x1af0` (interrupt request) is never set in the EXE; is there any other
  path that sets ScriptBootFlg (T[1]) ≠0 besides FUN_004251d0? (MAIN's `T[1]`
  branch then only matters for the load path via `G[910]`.)
* Thumbnail row order (top-down vs bottom-up DIB) and exact capture path when
  `*(eng+0x130)` is set (FUN_0043bc20 renders the layer tree into an 800×600
  surface first) — verify by decoding a real save.
* Exact meaning of many option bytes in app+0xe40..0xee3 (table §7.2): only
  consumers were located; the message/sound/input docs should name them.
* N_35 animation parameters (which value goes to which keyframe).
* Meaning of the mount record `param2` (−2 for PATCH/SCRIPT, −1 for game LPKs)
  — stored at mount+8, no reader found in the lookup path.
* Backlog serialisation format (eng+0x370 vt+0x10/+0x14) — needed only for
  byte-exact save compatibility.
* `app+0xd07`/`0xd0a` semantics beyond window-style toggling.
* Whether N_2a's error codes (from the catch block of FUN_0041dbe0) distinguish
  failure kinds (scripts only test zero/non-zero).
