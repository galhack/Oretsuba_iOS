# Text & message system (ORE_TUBA.EXE 1.01)

Scope: message manager (`DAT_00503258`), text-record markup, message-window
object (type 0x6f) and its reveal controller, the generic text object (type 5)
layout engine (wrap / kinsoku / ruby / tategaki), read-text tracking
(TEXT_LOG.DAT), backlog, choices, voice trigger, natives N_1cc..N_21a.
Fonts/glyph rasterisation: §9. Func_lib text loop: §10.

Conventions: `obj+0xNN` = byte offset; `[k]` in Ghidra output = int index
(k*4). "VERIFIED" = read from code; "GUESS" = inferred. All function
addresses are ORE_TUBA.EXE 1.01 VAs (`fn.sh ADDR` to print, or
`x86dis.py ADDR` for functions Ghidra did not export, e.g. the whole
0x45a000..0x45d800 text-object vtable).

---------------------------------------------------------------------------

## 1. Object graph

```
app  DAT_00503248
 +0x19c8 -> ScriptEngine (0x404 bytes, ctor FUN_004154b0)          [VERIFIED]
              +0x0ec int   nScripts      (from SCRIPT\SOB_INIT.DAT)
              +0x0f0 ptr   ScriptEntry[nScripts] (0x28 bytes each)
              +0x0f4 ptr   current ScriptEntry
              +0x0f8 char* TEXT_LOG.DAT file name (N_1e9)
              +0x0fc int   nVoiceChars   (VOICEINF.DAT)
              +0x100 ptr   VoiceChar[nVoiceChars] (16 bytes each)
              +0x104 int   nVoicePrefixes
              +0x108 ptr   VoicePrefix[n] {char* prefix; int charIdx}
              +0x10c char  voice file extension ("OGG")
              +0x138       SoundManager (DAT_00503630 = DAT_005034b0)
              +0x370       MessageManager  == DAT_00503258  (vtable 0x4edf10)
DAT_00503280  UI-object factory (creates msg windows N_2b2, text objects N_2b1 ...)
DAT_00503214  task/scene manager (per-frame tick objects)
DAT_0050325c  current "selection" object (type 6) registered by N_30b
```

The message manager is pure data/parsing: it owns the current text record,
the backlog ring, the current choice list and read-flag hooks. Rendering is
done by a *message window* object (type 0x6f) that pulls characters from the
manager every frame (§5). The manager's own drawing functions
(N_1d0/N_1d2/N_1d3.. → FUN_00432ce0) exist but are **not used by the shipped
scripts** (see usage table §8).

---------------------------------------------------------------------------

## 2. Text record format (argument of N_1ce)

Every line of a scene is one record in the SOB data area:

```
u8   type        always 0 in shipped scripts (57 865 records). 0 = line# is
                 in the current script's bitset; !=0 = line# is in the LAST
                 ScriptEntry's bitset (global/common pool)            [VERIFIED code]
u16  line#       index into the read-flag bitset (per script, 0-based)
[ '#' name-field '$' ]   optional (22 393 records have none = narration)
'$'* '&'*        any run of '$' and '&' after the name is skipped
text bytes ... '$' 00
```

Examples (cp932):

```
00 00 00 #鷹志\TAK1004$&　自分が普通じゃないってことに…$\0
00 03 00 #京\MIY1339$&<8>\「羽田君！」$\0
00 33 00 （ちなみに“事務処理”のひとつめは…）$\0                (no name)
00 xx xx #鷹志$&<10>\「ええっ！」$\0                           (name, no voice)
00 xx xx #$&　…………。$\0                                        (empty name)
```

### 2.1 Name field (FUN_00455aa0)  [VERIFIED]
`#` starts it; it runs to the next `$` (or NUL). The `$` is **overwritten
with NUL in place** (script memory is modified), then all following `$`/`&`
bytes are skipped; the body starts after them. The stored name string
(`mm+0x04`) is the raw `name\voice[\more]` text, split by `\` on demand:
FUN_004561f0(buf, idx) (= N_1d1):
* idx 0: whole raw string; idx 1: segment before first `\` = display name;
  idx 2: second segment = voice file id (e.g. `TAK1004`); idx n: n-th.
* returns length+1 (0 if segment absent).
* The voice id is copied (max 15 chars + NUL, else error 2) to
  msgwin+0x2a9 when the message is shown (§5.3) and read back by N_302.

### 2.2 `%` interpolation (name, body and choice strings)  [VERIFIED, unused]
`'%' u8 indirect u32 ptr` (6 bytes) is replaced by the C string at `ptr`
(indirect!=0: `*(char**)ptr`). When present the text is copied to a heap
buffer (`mm+0x0c` name / `mm+0x10` body), otherwise pointers alias script
memory. No shipped record uses it.

### 2.3 Body escape/tag syntax
Body scanners: FUN_00456540 (step one unit), FUN_004567f0 (step to next
stop), FUN_00456a90 (skip n tags / to end), FUN_00433000 (plain-text
extraction), FUN_00432ce0 (manager's own renderer), FUN_0045a990 (text
object put-char). Lead bytes are SJIS 0x81–0x9F/0xE0–0xFC everywhere
(Chinese patch widens to 0x81–0xFE).

| syntax | step code (FUN_00456540) | meaning (as executed by msg window FUN_00409090) | used? |
|---|---|---|---|
| DBCS / ASCII char | 0 | draw one glyph | yes |
| `\` | 0 | line break (text-object vtbl+0xb0) | only after tags |
| `<<` | 0 (advances 2) | literal `<` | no |
| `<N>` N = `[-]dec`, `0x`hex or `0b`bin, spaces/tabs/`,`/`(`/`)` skipped (FUN_004bf190) | 7, value → `mm+0x60` (N_1d9) | **font size delta**: font.lfHeight := \|current\| + N (cumulative within the message), line metrics recomputed; original size restored at message end/next message | ~352× (`<8>` 150, `<10>` 145, `<12>` 24, `<-4>` 10, and a few `<-5>`,`<-6>`,`<5>`,`<6>`,`<4>`,`<15>`,`<16>`,`<20>`), always at the start of the body |
| `<"str">` | 6, string → `mm+0x5c` (N_1d8) | draw `str` at cursor in one go (text vtbl+0xb8, no wrap) | no |
| a `\` directly after `>` of a tag | – | swallowed by the tag (so `<10>\` does **not** break the line) | always |
| `[` 00 ... | 8, `mm+0x64` = ptr to byte after `[\0`; block length = 8 + u16 at offset 6 | no-op in msg window (FUN_00409080); a following `\` is swallowed | no |
| `[` x (x≠0) | 0 | in FUN_00433000 copies `x` only (escape); FUN_00456540 treats `[` as plain char | no |
| `@@` | 0 | literal `@` | no |
| `@d…` (≤6 digits) | 0 (manager advances 1 byte only!) | put-char builds token `"@123"` and hands it to the font (gaiji?) | no — inconsistent, don't implement |
| `$` + NUL | 3 | wait for click (end of message) | every record |
| `$&` | 2 (consumes `$` only) | wait for click, then `&` → new page | no (in body) |
| `$` + other | 1 | wait for click, continue on same page | no |
| `&` | 4 | clear window (new page), continue | no (in body) |
| NUL | 5 | end of message | every record |

Read marking: when FUN_00456540/4567f0/456a90 return an end/stop code
(2,3,4,5 resp. NUL/&) and `mm+0x1c` (already-read) is 0, they call
`mm->vtbl[2](type,line#)` = FUN_004329a0 → FUN_00423ce0 = **set bit line#
in bitset1** (§4).

Plain text (N_1dc → FUN_00433000(buf, crlf=1)): drops tags and `@nnn`,
copies DBCS, `\` → `"\r\n"` (only if crlf), stops at NUL/`$`/`&`; returns
byte length+1 (buf may be NULL to measure). Used for save-slot excerpt
(FUN_0041dbe0: `【name】\n` + text, clipped to 140 bytes) and ssskip.sob.

---------------------------------------------------------------------------

## 3. MessageManager (`DAT_00503258` = ScriptEngine+0x370)

Ctor FUN_004328d0 → base FUN_00455340; vtable 0x4edf10 (base 0x4ee66c).

| off | type | meaning |
|---|---|---|
|+0x00|vtbl| [1]=0x432970 isRead(type,line) [2]=0x4329a0 markRead(type,line) [6]=0x432900 cursor:=origin [7]/[8] getX/getY [9] dtor |
|+0x04|char*| name field (raw `name\voice`) |
|+0x08|char*| body start |
|+0x0c/+0x10|char*| heap copies if `%` interpolation |
|+0x14/+0x18|int| name / body length+1 |
|+0x1c|u8| **already-read flag** of current line (computed at N_1ce / backlog load) — N_1e8 |
|+0x1d|u8| record type |
|+0x1e|u16| line# |
|+0x20|u8| message enable (N_1cf); N_1ce returns -1 and does nothing when 0 |
|+0x24|int| choice capacity (default 8; N_1ed) |
|+0x28|char**| choice strings (heap, `%`-expanded or added by N_1ef) |
|+0x2c|int| choice count (N_1f1) |
|+0x30|u8*| raw choice entry list (in record) |
|+0x38|u8| "message shown since choices were set" |
|+0x39|u8| choice type: 0 = current script bitset2, 1 = common entry bitset2, 2 = none (default) |
|+0x3a|u16| choice base id (bit index base) |
|+0x3c|u8| number of choices that have read-bits |
|+0x3d|u8| selected choice index |
|+0x3e|u8| choices enable (N_1ec, default 1) |
|+0x40|ptr*| backlog ring (entries, 0x24 bytes each) |
|+0x44|int| backlog capacity (N_1cc; config byte app+0xe5a, +1 if ≠0) |
|+0x48|int| ring write index |
|+0x4c|int| entries used (N_1dd) |
|+0x50|u8| backlog enabled (N_1cd) |
|+0x54|int| backlog view cursor (N_1de); = used-1 after a push |
|+0x58|u8*| current read pointer into body |
|+0x5c|char*| last `<"…">` string (N_1d8) |
|+0x60|int| last `<N>` value (N_1d9) |
|+0x64|u8*| last `[\0` block (+2) |
|+0x68..+0x8c| | own renderer state (N_1d0): +0x68 surface, +0x6c dirty-rect sink, +0x70 font, +0x74 use FUN_0044e6f0, +0x75 surface has alpha, +0x76 full-width cells, +0x78 char spacing, +0x7c/+0x80 origin x/y, +0x84/+0x88 cursor x/y, +0x8c/+0x90 bookmark (N_1e5/N_1e6) |

### 3.1 N_1ce(text) = FUN_00455a30  [VERIFIED]
```
if !enable(+0x20) return -1
type=rec[0]; line=u16 rec[1..2]
+0x1c = isRead(type,line)          // FUN_00423c90 bit test
r = parse(rec+3)                   // FUN_00455aa0: name/body split, % expand
if r==0: attachChoicesToPrevBacklog() (FUN_004557f0); pushBacklog() (FUN_00455590)
+0x58 = body; +0x38 = 1; cursor := origin
return r
```

### 3.2 Backlog (history.sob)  [VERIFIED]
Ring of `capacity` pointers. Push (FUN_00455590, only if +0x50 && capacity):
entry = new 0x24 bytes:
```
+0x00 char* name copy   +0x04 len     +0x08 char* body copy  +0x0c len
+0x10 u8*  choice blob  +0x14 size    +0x18 choice count
+0x1c u8   selected choice            +0x1d type  +0x1e u16 line#
+0x20 u8   choice type  (+0x39)       +0x22 u16 choice base (+0x3a)
```
Write at index +0x48 (old entry freed), +0x48 wraps, +0x4c=min(+0x4c+1,cap),
+0x54 = +0x4c-1. Choice blob is attached to the *previous* entry when the
next N_1ce arrives (FUN_004557f0): sequence of `[u8 size=len+3][u8 0][str\0]`.

Load entry k (0 = oldest) into the manager: FUN_00456380(k): index =
(+0x48 - +0x4c + k) mod cap; sets name/body/+0x58, choices, recomputes
+0x1c read flag, +0x54 = k, cursor reset. Wrappers: N_1df prev (k-1),
N_1e0 next (k+1), N_1e1 oldest, N_1e2 newest, N_1e3(k) absolute (clamped),
N_1e4(d) relative; each returns 0 on success, else -1 and calls
FUN_004321a0 (re-push choice read state to selection object).
N_1da/N_1db = can-go-prev / can-go-next.
After loading, history.sob displays the entry through its own message
window (N_2fb = show page instantly).

### 3.3 Choices  [VERIFIED]
N_1eb(rec) = FUN_00455ec0; record layout (example s01_02c @3ff1):
```
00          type   (0 cur script, 1 common, 2 none) -> +0x39
00 00       u16 base id                              -> +0x3a
02          count n (clamped to capacity)            -> +0x3c, +0x2c
00          needs-% flag (0 = strings used in place)
n × { u8 size(=len+3), u8 pct, cp932 string \0 }     "国と民衆のために", "アスカ姫のために"
```
Returns -1 if choices disabled, -2 if the previous backlog entry already
has choices; on <1 calls FUN_004321a0 which calls
`DAT_0050325c->vtbl[0xa0](bitset2*, base, 0)` so the selection object can
tint already-chosen options. N_1f4(i) → string ptr, N_1f3(i,buf,size) copy,
N_1f5(i) select: stores index (+0x3d or prev backlog entry +0x1c) and sets
bit (base+i) in **bitset2** (FUN_00423d80); N_1f6 → selected (0xff none);
N_1f7(i) → was option i ever chosen (FUN_00423d30). N_1ef(str) appends a
choice (max 253 bytes), N_1f0 clears, N_1ed(n) capacity, N_1ee resets base.

---------------------------------------------------------------------------

## 4. Read-text tracking (skip "already read")  [VERIFIED]

* `SCRIPT\SOB_INIT.DAT` (FUN_00424440): `u16 n; n × {u8 len; char name[len]
  (NUL incl.); u16 nbits1; u16 nbits2}` — 412 entries (MAIN, GAMEINIT, …,
  scenes). Builds ScriptEntry[n]:
  ```
  +0x00 char* name  +0x04 u8 namelen  +0x05 u8 loaded
  +0x08 Bitset1 (read text lines)   {+4 nbits, +8 nbytes, +0xc u8* data}
  +0x18 Bitset2 (chosen options)    same layout
  ```
  bit test = `data[i>>3] & (1<<(i&7))`.
* Record type 0 uses the *current* script's entry, type≠0 the *last* entry
  (index n-1) — a shared pool (unused).
* `isRead` (FUN_00423c90) at N_1ce; `markRead` (FUN_00423ce0) when the text
  reaches its end (§2.3).
* Persistence: `TEXT_LOG.DAT` (name set by N_1e9 in gameinit.sob, loaded by
  N_1ea = FUN_00424180, written by FUN_00423e30):
  ```
  "TLOG" u32 nVoiceChars; nVoiceChars × {u32 volume; u8 voiceOn; u8 flag}
  u32 hasScripts; [u32 packedSize; u32 nScripts; packed blob]
  blob (FUN_00495420/FUN_00495710 compression) = nScripts ×
     {u8 nlen; name; u32 nbits1; u32 nbytes1; bytes; u32 nbits2; u32 nbytes2; bytes}
  ```
  On load an entry is matched by name and each bitset is restored only if
  its bit count is unchanged.

---------------------------------------------------------------------------

## 5. Message window object (type 0x6f)

Created by **N_2b2** (FUN_00408ec0, 0x2d0 bytes, ctor FUN_00409870,
vtable 0x4ed950), derived from the text object (type 5, vtable 0x4ee6b4,
§6). Extra fields:

| off | meaning |
|---|---|
|+0x298| MessageManager (DAT_00503258) |
|+0x29c| task manager (DAT_00503214) |
|+0x2a0| RevealController (0x34 bytes, vtable 0x4ed924, created on first N_2fa) |
|+0x2a4| click-wait cursor animation (FUN_004574e0 class, built from +0x2bc) |
|+0x2a8| u8 "click completes page" (N_2f8, default 1) |
|+0x2a9| char[16] voice id of current line (N_302) |
|+0x2bc| cursor animation source (N_2fd; advinit: N_20d "SYS/MS_CURSR.ELG") |
|+0x2c0| name window (type 5 text object, N_2fe); moved with the window (FUN_004099a0), receives the same bg style |
|+0x2c4| skip button (GMP part #5 of SYS/MS_FRAME.GMP, N_2ff) |
|+0x2c8| u8 name alignment mode (N_2fe arg2; advinit uses 1) |
|+0x2c9| u8 cursor visible |
|+0x2ca| u8 name window has a name |

advinit.sob setup (args in push order; `N_2b2(x,y,w,h,font,?,frameB,frameA,frameG,z)`):
```
N_2b2(4,466, 0,0, G[900], 0, "SYS/MS_FRM_B.ELG","SYS/MS_FRM_A.ELG","SYS/MS_FRM_G.ELG", 1500) -> G[810]
N_2f1(win, 1,0,2,1,2,0)       fullwidth-cells=1 charSpacing=0 lineSpacing=2 autowrap=1 wrapReserve=2 rubyGap=0
N_2e6(win, 28,13,16,34, 2,2,2,2)   margins L,T,R,B = 28,13,16,34 ; edge pads = 2,2,2,2
N_2b1(4,427, 0,0, G[900], 0, "SYS/NM_FRM_*.ELG", 1500)       -> G[811] (name box)
N_2f1(name, 1,0,0,0,0,0)      fullwidth cells, no wrap
N_2e6(name, 23,7,23,7, 2,2,2,2)
N_2fe(win, name, 1)
N_20d(... "SYS/MS_CURSR.ELG" ...) -> G[816];  N_2fd(win, G[816])
N_208("SYS/MS_FRAME.GMP",0,1100,0); N_20c(0,0,G[817],...)  -> 10 frame buttons
N_2ff(win, G[817][5])         skip button
```
(Frame images: arg B → +0x244, A → +0x248 (its size defines the window
surface), G → +0x24c; see graphics doc. GUESS: G = graphic, A = alpha,
B = ? .)

### 5.1 Natives on the window (outside N_1cd..N_21a, listed for completeness)
| native | call | effect |
|---|---|---|
|N_2fa(win, b)|FUN_00409b00 → FUN_00409570(ctrl, 0, b)|start showing the message currently in the manager; b = allow auto-skip of read text |
|N_2fb(win)|FUN_00409c90 → FUN_00409570(ctrl,1,1)|show whole message instantly (history) |
|N_2fc(win, m)|FUN_00409d20|m=0 reveal to next stop now; 1 reveal everything until a wait/end code; 2 finish (FUN_00409240); 3 leave wait state (hide cursor, state=1) |
|N_2fd(win, anim)|FUN_00409ac0|set click cursor animation |
|N_2fe(win, textobj, mode)| |attach name box |
|N_2ff(win, btn)| |attach skip button |
|N_300(win)| |controller state (+0x24) |
|N_301(win)| |controller skip flag (+0x28) |
|N_302(win, buf)| |copy voice id, returns 1 if non-empty |
|N_2f8(win, b)| |+0x2a8 |

### 5.2 RevealController (0x34 bytes, vtable 0x4ed924)  [VERIFIED]
A task registered with DAT_00503214; `vtbl[2]` = **FUN_00409450(now_ms)**
runs every frame. vtbl[9]/[10] = pause/resume (pause counter +0x18,
pause time +0x1c, shifts +0x08 base time — FUN_00458280/FUN_00420030).

| off | meaning |
|---|---|
|+0x20| window |
|+0x24| state: 0 armed, 1 revealing, 2 waiting for click (cursor shown), 3 message ended (task paused), 4 finished instantly |
|+0x25| last step code (0xff error) |
|+0x26| task paused flag |
|+0x27| line already read (mm+0x1c) |
|+0x28| auto-skip this message |
|+0x29| u8 ms per character |
|+0x2a| u8 "no typewriter" (reveal whole segment per tick) |
|+0x2c| next reveal time (ms) |
|+0x30| saved font size for `<N>` tags (0 = unchanged) |

**Start (FUN_00409570(ctrl, instant, allowSkip))**
1. restore font size if `<N>` changed it; clear page (vtbl+0xa8); hide
   cursor; show window if hidden.
2. name box: clear; name = segment 1; if non-empty: draw it (mode<4:
   `name->vtbl[0x118](name, mode)` (aligned print), else `vtbl[0x11c]`
   with full rect), show box; else hide box.
3. voice id = segment 2 → +0x2a9 (len ≥16 → return 2).
4. +0x27 = mm+0x1c.
5. if `instant`: FUN_00409240 (render whole page) and return.
6. `skipKey` = FUN_0041af60(), `skipBtnOn` = skip button state in {3,4,5}
   (these two live in registers; Ghidra shows them as `unaff_EBX`).
   `+0x28 = cfg[0xe59] & read & allowSkip`.
   *unread*: if cfg[0xeb9] → skipKey = 0; if cfg[0xe45] the skip button
   is disabled (`vtbl+0x2c(0)`) and, if toggled, un-toggled (`vtbl+0x8c(0)`),
   so skipping stops at the first unread line; else enabled.
   +0x2a = cfg[0xe55], +0x29 = cfg[0xe56].
   *read*: +0x2a = cfg[0xe57], +0x29 = cfg[0xe58]; if +0x28 and (advance
   input or app+0x1af1 "modal active") → +0x28 = 0; skip button enabled.
7. if !(+0x28 | skipBtnOn | skipKey): state 0, next=0, resume task;
   else render whole message (FUN_00409240), state 4.
   Config meanings in §11.

**Tick FUN_00409450(now)**
```
if !win.active (vtbl+0x5c) return
skipKey = FUN_0041af60()      // DirectInput: Shift (0x2a/0x36) or, if cfg eb8, Ctrl (0x1d/0x9d)
skipBtn = skipButton && skipButton.state(+0x3f) in {3,4,5}
if (skipKey && (!cfg[0xeb9] || read)) || skipBtn:
     FUN_00409240 (render rest of page instantly); state = 4; return
if state==1 && win+0x2a8 && advancePressed():  FUN_00409090(all=1); return
if state==2: if !advancePressed() return; hide cursor; state=1
if next <= now: state=1; FUN_00409090(all=+0x2a); next = now + (+0x29)
```
`advancePressed` = FUN_004093c0: window visible & active & parent layer not
paused, and (input action 0x13 or 4 fired, or left mouse click not over a
button of the layer (FUN_00408910 hit test)).
**Text speed therefore = at most one character per tick**; with delay
smaller than the frame period the speed is one character per frame.

**Reveal step FUN_00409090(all)**: loop on FUN_00456540 (manager step):
* 0 char: `\` → newline (vtbl+0xb0) else put-char (vtbl+0xb4); if result
  ≥1 (page full): FUN_00456a90(-1) drops the rest of the page and code is
  forced to 3. Without `all`, return after one drawn glyph.
* 1/2/3: state=2, show cursor (`cursor->vtbl[0x28]`), return (wait click).
* 4: clear page (msgwin vtbl+0xa8 = FUN_00409a40: text-object clear
  FUN_0045a800 + hide name box) and continue.
* 5: pause task, state=3, restore font size; return.
* 6: `vtbl+0xb8(str,-1,-1)` (draw string); 7: font size += N, vtbl+0xf0
  (relayout line metrics); 8: ignore.
FUN_00409240 is the same loop without stopping at 1/2/3 (instant page).

---------------------------------------------------------------------------

## 6. Text object layout engine (type 5; base class vtable 0x4ee7f0)

Base ctor FUN_0045b9f0 defaults in brackets. Most methods are only in raw
disassembly (`x86dis.py`).

| off | meaning |
|---|---|
|+0x28| font (LOGFONT inside: font+0x0c = lfHeight, \|h\| = glyph cell size) |
|+0x2c| ruby font [0] |
|+0x30/+0x34| box width / height (from frame image) |
|+0x38| bg RGB, +0x3c bg alpha [0x80], +0x3d bg mode [1] (vtbl+0x84; cfg e85/e88/e86 via FUN_0041d770; N_2e8) |
|+0x40/+0x44/+0x48/+0x4c| margins L,T,R,B [8,8,8,8] (vtbl+0x88, N_2e6 args1-4) |
|+0x50| minimum line height [0] |
|+0x54| current line height |
|+0x58| line advance |
|+0x5c/+0x60/+0x64/+0x68| edge pads [2,2,2,2] (vtbl+0x8c, N_2e6 args5-8) |
|+0x6c/+0x70| cursor x / y |
|+0x75| full-width cell mode (N_2f1 a1) |
|+0x76| vertical writing (tategaki) |
|+0x77| auto-wrap [1] (N_2f1 a4) |
|+0x78| line spacing (N_2f1 a3) |
|+0x7c| character spacing (N_2f1 a2) |
|+0x80| ruby gap [-2] (N_2f1 a6) |
|+0x84| wrap reserve in cells [2] (N_2f1 a5) |
|+0x88| wrap threshold |
|+0x90| indent (vtbl+0x10c FUN_0045d210 sets = cursor-margin; vtbl+0x110 clears) |
|+0x94| pending ruby string (vtbl+0x114, FUN_0045d250; **no caller found**) |
|+0x98| u8 "next line would overflow" |
|+0x99| u16 previous character (kinsoku) |
|+0xfc| drawing surface; +0x180 dirty-rect sink |

Formulas (horizontal; vertical swaps axes, see code):
```
lineH   = |font.h| + (rubyFont ? rubyGap + |ruby.h| : 0);  lineH = max(lineH, minLineH)   // +0x54  (FUN_0045d0c0)
advance = lineH + padTop(+0x60) + padBottom(+0x68)                                     // +0x58
wrapX   = boxW - |font.h| * wrapReserve - marginR                                       // +0x88  (FUN_0045d170)
clear page (FUN_0045a800): x = marginL, y = marginT, indent = 0, ruby = 0
newline (FUN_0045a890): if y + lineH + lineSpacing + advance > boxH - marginB -> return 1 (page full)
                        y += lineSpacing + advance; x = indent + marginL
```
**Put-char FUN_0045a990(&p)** (vtbl+0xb4):
1. token = DBCS pair, `@@`, `@digits`, or 1 byte.
2. if autowrap and x > wrapX (kinsoku zone):
   * let prev = previous char (+0x99), cur = token;
   * if prev ∈ OPEN or cur ∈ CLOSE or cur ∈ SMALL → break only if the glyph
     does not fit: `x + |font.h| > boxW - marginR`;
   * otherwise break now.
   A break calls newline; if that reports page full → return 1 (char not
   consumed); else result -1 (wrapped) and continue.
   Tables (DBCS / SBCS):
   * OPEN (0x4fc57c / 0x4fc578): `‘“（〔［｛〈《「『【￥＄￡` / `\$([{｢`
   * CLOSE (0x4fc56c / 0x4fc568): `。，、．：；゛゜ヽヾゝゞ々’”）〕］｝〉》」』】°′″℃￠％‰` / `%),:;]}｡｣ﾞﾟ`
   * SMALL (0x4fc574 / 0x4fc570): `・？！ーぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ` / `!.?､･ｧｨｩｪｫｬｭｮｯｰ`
3. draw (font->FUN_0044e6f0(surface, x, y, token, &rect, &adv)):
   * DBCS and full-width mode: measure (font vtbl+0x28), x' = x + (h-w)/2,
     y' = y + (lineH - h); advance = h (fixed cell = font height);
   * else x' = x, y' = y + (lineH - h); advance = rect.right - x.
   Glyphs are bottom-aligned in the line box (ruby lives above).
4. ruby (if +0x94): centred over the glyph: rx = x + adv/2 - rubyW/2,
   ry = y - rubyGap - |ruby.h|; then cleared.
5. x += charSpacing + advance; dirty-rect notify.
Return 0 drawn, -1 drawn after wrap, 1 page full (not drawn).

Name-box print (vtbl+0x118, FUN_0045d270(str, mode)): measures the string
and calls vtbl+0xb8(str, x, y) with, horizontally, mode 0 = centred in the
padded box both ways, 1 = x 0 / current y, 2 = right aligned, 3 = x from
cursor (−1); advinit uses mode 1. (Vertical table at 0x45d3c0 similar.)

Message window effective values: font = G[900] ("ＭＳ ゴシック" 22 px per
gameinit, the default font N_a6; passed to N_2b2 as args[4]) → cell 22,
lineSpacing 2, pads 2+2 → line pitch 22+2+2+2 = 28 px; wrap reserve 2
cells; full-width cells (every DBCS char advances exactly 22 px +
charSpacing 0). Text-object glyphs are drawn with FUN_0044e6f0
(ARGB "over" onto the window's own surface, §9). `<10>` → 32 px,
`<-6>` → 16 px for that message. (GUESS: font+0x58 extra line spacing is
not used by the text object; only by the font's own multi-line draw.)

---------------------------------------------------------------------------

## 7. Voice trigger (FUN_004326a0 = N_2a4)  [VERIFIED]

`SCRIPT\VOICEINF.DAT`: `u8 len, ext ("OGG\0"); u32 nPrefix (99);
u32 nChar (34); nChar × {u8 len, name(cp932, NUL); u8 n2, [n2>1: n2 bytes
alt string]; u32 k; k × {u8 len, prefix\0}}` → VoiceChar 16 bytes
`{+0 name, +4 alt, +8 int volume, +0xc u8 voiceOn, +0xd u8 flag}`
(volume/voiceOn persisted in TEXT_LOG.DAT), VoicePrefix {prefix, charIdx}.

`N_2a4(ch, voiceId, n, slotPrefix[n])`:
1. find first prefix p with `voiceId` starting with it (e.g. `TAK1004` →
   `TAK`) → -3 if none; ch=-1 → return prefix index; ch<-1 → charIdx.
2. if sound+0x203 (voice on) and VoiceChar.voiceOn: slot = index of p in
   slotPrefix (or -1); play `VOICE\<voiceId>.OGG` on channel `ch`
   (FUN_00453560, flag 2 if slot≥0 and app+0xe48), volume = VoiceChar
   volume (FUN_00453430); return slot (or p if n==0); -4 on play error.
   else return -2.
func_lib calls it with ch=5, the 6 on-screen character slots; the returned
slot drives panning (from the sprite x) and lip-sync (N_214) — see §10.

---------------------------------------------------------------------------

## 8. Natives N_1cc..N_21a

`mm` = DAT_00503258. Args in push order. "uses" = script files (scenes
counted). ✗ = not used by any shipped script.

| N | addr | args → ret | behaviour | uses |
|---|---|---|---|---|
|1cc|431e40|(u8 cap)|backlog capacity FUN_00455520 (frees all)|✗ (engine sets it from cfg e5a)|
|1cd|431e60|(b)|mm+0x50 backlog enable|advinit, gameinit, func_lib|
|1ce|431e80|(rec) → int|set message (§3.1)|57 865 × scenes|
|1cf|431ea0|(b)|mm+0x20 message enable|func_lib, gamereset, scskip, ssskip|
|1d0|431ec0|(surface,x,y,sink,font,useAlt,fullwidth,spacing)|manager renderer setup (FUN_004329d0) |✗|
|1d1|431f50|(buf,idx) → len+1|name segment (FUN_004561f0)|✗|
|1d2|431f70|(x,y,idx,surf,font,mode)|draw name segment (FUN_00432a40)|✗|
|1d3|432030|(all) → code|step + draw (FUN_00432bd0)|✗|
|1d4|432050|() → bool|redraw from start (FUN_00432c40(-1,1))|✗|
|1d5|432070|(n,draw)|reset + skip n tags (FUN_00456a90)|✗|
|1d6|432090|() → bool|redraw up to current pos|✗|
|1d7|4320b0|(&x,&y)|cursor pos|✗|
|1d8|4320e0|(buf) → bool|copy `<"…">` string|✗|
|1d9|432110|() → int|last `<N>`|✗|
|1da/1db|432120/432140|() → bool|backlog can prev / can next|✗|
|1dc|432160|(buf) → len+1|plain text with CRLF (FUN_00433000)|ssskip|
|1dd|432180|() → int|backlog count|advinit, func_lib, history, sskip|
|1de|432190|() → int|backlog cursor|✗|
|1df/1e0/1e1/1e2|432210…270|() → 0/-1|load prev/next/oldest/newest backlog entry|1e2: func_lib, scskip, ssskip|
|1e3|432290|(k) → 0/-1|load entry k|history|
|1e4|4322c0|(d)|load cursor+d|✗|
|1e5/1e6|4322f0/432310|() → bool|bookmark / restore read pos|✗|
|1e7|432330|()|clear selection hint, clear message+choices (FUN_00455430)|func_lib|
|1e8|432360|() → u8|current line already read (mm+0x1c)|func_lib|
|1e9|432370|(name)|TEXT_LOG.DAT file name (FUN_00423dd0)|gameinit|
|1ea|432390|() → 0/1/2|load TEXT_LOG.DAT (FUN_00424180)|gameinit|
|1eb|4323b0|(rec) → int|set choices (§3.3)|24 scenes via func_lib|
|1ec|4323e0|(b)|mm+0x3e choices enable|func_lib, gamereset, scskip|
|1ed|432400|(n)|choice capacity (FUN_00455e10)|✗|
|1ee|432430|()|clear choice base/count|✗|
|1ef|432460|(str) → int|add choice|✗|
|1f0|432490|()|clear choices (FUN_00455dc0)|✗|
|1f1|4324c0|() → int|choice count|func_lib, ssskip|
|1f2|4324d0|(i,x,y,surf,font,mode)|draw choice i (FUN_00433190)|✗|
|1f3|432590|(i,buf,size) → 0..3|copy choice i|✗|
|1f4|4325b0|(i) → char*|choice string|func_lib, ssskip|
|1f5|4325d0|(i) → 1|select choice i (+ mark bitset2)|func_lib + 24 scenes|
|1f6|4325f0|() → u8|selected (0xff none)|func_lib|
|1f7|432610|(i) → bool|choice i chosen before|✗|
|1f8|41f220|(file, b) → obj|load a "LAG" (magic 0x47414c) object into list DAT_00503520 (type 13000; FUN_0046ec50/0046ed00) GUESS animation data|logo|
|1f9|41f250|(&obj) → int|free it (sets *obj=0)|logo|
|1fa|41f290|()|free all type-13000 tasks and list 0x503520|gamereset|
|1fb|41f2e0|(obj,file,b)|reload (FUN_0046ed00)|✗|
|1fc|41f320|(surface, a1,a2,a3, loop, b5, b6)|play it on a surface: task 0xf8 bytes (vtable 0x4ede98, FUN_00457390; frame stepping FUN_00457250: per frame {idx, ms, s16}, loop count +0x2c)|logo|
|1fd|41f4b0|(obj) → u8|playing state (+0xf4)|✗|
|1fe|41f560|(u32 id) → obj|load resource id into list DAT_00503508 (type 15000, FUN_0046f170/0046f290)|func_lib|
|1ff|41f580|(&obj)|free it|func_lib|
|200|41f5c0|()|free all type 15000|✗|
|201|41f610|(obj,id)|reload|✗|
|202|41f640|(obj,k) → u16/u32|field k of it (0:+0xe,1:+0x18,2:+8,3:+0xa)|✗|
|203–207|41f690…|…|"hit/part list" DAT_005034e8 (load from image, count DAT_005034f0, arrays of u32,u32,{s16,s16,u8})|✗|
|208|41f890|(file, a1, z, b)|load GMP layout (e.g. SYS/MS_FRAME.GMP) into DAT_00503268/0x503270.. creating UI objects (FUN_0040e8a0)|11 system scripts|
|209|41f8d0|()|finish/free GMP loader state|same|
|20a|41f8e0|() → n|GMP part count (DAT_00503270)|9|
|20b|41f8f0|(i,&obj,&x,&y,&b)|part i|✗|
|20c|41f950|(first,n,objArr,xArr,yArr,bArr)|bulk copy parts (n=0 → all)|11|
|20d|41fa70|(…12 args, "SYS/MS_CURSR.ELG" …)|create animated sprite into DAT_005034d0 (FUN_0040f070; types 0x32c9/0x32ca) — used for the click cursor|advinit|
|20e/20f|41fbc0/41fbe0|(obj,x)|animation control (FUN_004763e0/FUN_00476610) GUESS start/stop|func_lib|
|210|41fb80|(&obj)|free sprite|func_lib|
|211|41fc00|()|free all sprites|func_lib|
|212|41fc60|(…)|re-init sprite (FUN_004761a0)|✗|
|213|41fd40|(…)|create blink/anim task (FUN_004574e0/4576c0, same class as the click cursor)|✗|
|214|41fe40|(mouthObj, soundObj) → task|**lip-sync**: 0x30-byte task (vtable 0x4edec4, FUN_00457e50) bound to a playing voice (N_2a3(5)) and a mouth animation (G[827][slot]); fails if sound object +0x4a==0|func_lib (text loop)|
|215|41ff60|(p) → p+8|pointer helper|func_lib|
|218|41abb0|(u16 n)|allocate n stopwatches (DAT_00503610; 16-byte objects vtable 0x4ee010)|gameinit (once)|
|219|41abd0|(u16 i) → bool|stopwatch i := now (timeGetTime/GetTickCount)|38× (config, bgmmode, func_lib …)|
|21a|41abf0|(u16 i) → ms|elapsed ms of stopwatch i|40×|

Func_lib wrapper after each line (`call L+2c3c(0)`): see §10.

---------------------------------------------------------------------------

## 9. Fonts and glyph rendering

(Headless re-decompilation of the font vtables:
`/private/tmp/claude-501/-Users-a-ai/e18de769-5e14-4a40-931e-518607b4d1a9/scratchpad/fontagent/out1.c` (session scratch, may be gone).)

### 9.1 Font object (common base, ctor FUN_004508d0)  [VERIFIED]
| off | meaning | default |
|---|---|---|
|+0x04/+0x08| HFONT / memory HDC | |
|+0x0c| LOGFONTA: lfHeight = −size (\|lfHeight\| = em px = "font.h" everywhere above) | −16 |
|+0x14/+0x18| escapement/orientation (rotation, 1/10°; 2700 = vertical) | 0 |
|+0x1c| lfWeight 400 / 700 bold | 400 |
|+0x20..+0x22| italic/underline/strikeout | 0 |
|+0x23| lfCharSet = 0x80 SHIFTJIS (Chinese patch hook → 0x86 GB2312) | |
|+0x26| lfQuality 3 | |
|+0x28| lfFaceName[32] | |
|+0x48| AA: 0 GGO_BITMAP, 1 GGO_GRAY4, 2 GGO_GRAY8 | 1 |
|+0x49..4b| text RGB | 255,255,255 |
|+0x4c| effect: 0 none, 1 shadow, 2 edge | 0 |
|+0x4d..4f| edge/shadow RGB | 0,0,0 |
|+0x50/+0x54| shadow dx/dy | 0 |
|+0x58| extra line spacing for '\n' in the font's own draw | 0 |

Two classes: **glyph font** (GetGlyphOutlineA, vtbl 0x4ee580, ctor
FUN_0044aef0, 0x20b8 bytes, typeId 1) used by message/text objects, and a
**TextOut font** (GDI TextOutA, vtbl 0x4ee600, typeId 0; edge = 8 TextOutA
at ±1, shadow = 1 offset TextOutA). Glyph-font vtable: +0x0c init(size,
bold, italic, face), +0x10 recreate (CreateFontIndirectA+GetTextMetricsA),
+0x14 face, +0x18 size, +0x1c bold/italic, +0x20 rotation, +0x24 AA mode,
**+0x28 measure(text, SIZE*, offX*, offY*, withEffect)**, **+0x2c
draw(surface, x, y, text, RECT* dirty, POINT* endPen)**; non-virtual
FUN_0044e6f0 drawOver (same args, ARGB over), FUN_0044bc80 getGlyph,
FUN_0044b410 gradient, FUN_0044bb80 edge(density, radius).

### 9.2 Glyph raster (FUN_0044bc80)  [VERIFIED]
* code = `(lead<<8)|trail` passed raw to GetGlyphOutlineA (charset picks
  the code page) → a port must decode cp932 (or GBK for the CHS scripts)
  to Unicode itself.
* GRAY4 levels 0..16 → alpha `min(v*16,255)`; GRAY8 0..64 → `min(v*4,255)`;
  fallback if GGO_GRAY fails: GGO_BITMAP at 4×/8× + box filter.
* glyph = {ox = gmptGlyphOrigin.x, oy = tmAscent − gmptGlyphOrigin.y,
  advance = GetTextExtentPoint32A(char).cx (not gmCellIncX), pitch, w, h,
  bits}. Pen = top-left of the line cell, baseline = pen.y + tmAscent.
* bold = GDI weight 700; advance measured on the regular font:
  `cx + 1 + cx/50`. Optional software bold (max-filter n = size/50+1)
  only when DAT_00503880 == 0 (GUESS: non-NT) or supersample fallback.
* **edge** glyph (separate cache entry): grayscale dilation, radius r
  (default 2), octagonal distance `d = b + (a2>>2) + (a2>>6) − (b>>7) −
  (b>>5)` (a=min(|dx|,|dy|), b=max, a2=a+a/2), every offset with d ≤ r
  adds `src * ((r−d+1)*density*norm>>8) >> 16`, saturating; norm =
  0x10000/Σ(r−d+1) (r=2 → 0x750), density default 0x400 (gain ×4).
  Output (w+2r)×(h+2r), origin −r.
* cache: 256 buckets, LRU, capacity 512 glyphs; key = edge<<31 | bold<<30
  | italic<<29 | (size&0x3ff)<<16 | code. Colour/effect/gradient applied
  at blit time.
* gradient (optional): per-row colour table from c1 (top) to c2 (bottom)
  over \|h\| rows, main glyph only.

### 9.3 Draw / blend  [VERIFIED]
Per char: effect 2 → edge glyph in edge colour; effect 1 → main glyph in
edge colour at +(dx,dy); then main glyph in text colour; pen += advance.
32-bpp BGRA surfaces. Normal draw: `d += (c−d)*(a+1)>>8` per channel,
dest alpha kept. drawOver (used by text objects): ARGB "over":
`w = 255*sa/(sa + da*(1−sa))` (255 if da=0), `rgb = d + (c−d)*(w+1)>>8`,
`outA = 255 − (255−sa)(255−da)/255`.

### 9.4 Fonts configured by scripts
* gameinit: **G[900] main message font**: N_a4("ＭＳ ゴシック", 22),
  default font (N_a6), line spacing 2, white, gradient colours
  (0xffeb00, 0xff4646) stored (enabled only by config); G[902] Gothic 14;
  G[837] TextOut "ＭＳ Ｐゴシック" 16 bold, yellow, black edge; G[849]
  Gothic 19 white, edge colour (46,138,192) (tooltips); G[848] history.
* user config category 2 (N_8 set / N_9 get, app+0xe60..): 0 face
  (default "ＭＳ ゴシック"), 1 AA mode (+0xe80, def 1), 2 gradient on (+0xe81,
  def 0), 3 effect mode (+0xe82, def **2 = edge**), 4 bold (+0xe83, def 0),
  6/7/8 window bg mode/alpha/colour (+0xe85 def 1, +0xe86 def 140, +0xe88
  def 0). Applied by FUN_0041d770 (native N_c): effect = cfg, shadow
  offset forced (2,2), gradient = cfg, AA = cfg, re-init if bold/face
  changed; window/name/selection bg via vtbl+0x84. (No literal
  "cf_bold"/"cf_edge" strings exist in the EXE; these are the equivalents.)
* Default look: 22 px MS Gothic, GRAY4 AA, white, soft black edge r=2,
  no gradient, window bg mode 1 colour 0 alpha 140.

### 9.5 Chinese patch impact
CreateFont*/EnumFontFamiliesExA forced to GB2312; GetGlyphOutlineA hook
remaps 9 SJIS title glyphs; ~100 lead-byte upper bounds 0x9f/0xfc → 0xfe
(its EXE is a different build: addresses +0x10 in 0x44a..0x45x, +0x20 in
0x41d..0x43e; e.g. our put-char/kinsoku code, the message renderer
0x432d7e/0x432dab, font measure/draw). With GBK text all bytes 0x81..0xFE
are lead bytes; the kinsoku tables stay SJIS (so CJK punctuation kinsoku
silently stops working for GBK text — GUESS from the unchanged tables).

---------------------------------------------------------------------------

## 10. func_lib text routine `L+2c3c(noWait)` (called after every N_1ce)

Scenes: `N_1ce(rec); if !G[786] { if (L+2c3c(0)) return; }` (G[786] =
scene-skip mode, then the scene jumps to "ScSkip"/"SsSkip"). Before a
choice the question line uses `L+2c3c(1)` (no click wait). Lifted with an
expression-folding SOB lifter (scratch `libagent/`). The typewriter,
tags, click handling and read marking are all native (§5); the script only
starts the window, plays voice/SE and polls state each frame.

```
int ShowText(noWait):
  if G[905]:                         // restore / fast-forward after load or "back to choice"
     if G[906]==G[908]: N_1cf(1); N_1ec(1)
     if --G[906] > 0: return 0       // replay silently
     if G[908]==-2: N_1cf(1); N_1ec(1); N_1e2()      // current = newest backlog entry
     ... rebuild scene from saved state (N_4e copy 780 bytes G[784]->G[757], redraw
         character slots/bg/effects, transition) ...; G[905]=0; G[814]=G[819]=0; fx2=0
  else: N_4e(G[757],G[784],780); G[806]++; fx2=1
  L+22fdc(7, N_1dd()>1)              // backlog button enabled iff >1 entries
  if !G[819] && !N_9(0,9): L+8218(1,0)   // show system button bar
  N_2fa(G[810], fx2)                 // start line in the message window
  G[818][5] = N_2d0(G[817][5]); if !G[818][5]: G[915]=0   // skip button disabled -> skip off
  G[814]=1
  N_302(G[810], G[847])              // voice id -> buffer
  if N_300(win)==4:                  // whole message already shown (skip)
     YIELD one frame; poll commands; (click cancels skip via L+f4); goto finish
  if *G[847]: if N_9(3,4) PlayVoice() else *G[847]=0          // voice on/off
  if G[850]: if N_9(3,2) { N_299(7,G[850]); N_29c(7,0) } else G[850]=0   // per-line SE
  loop each frame:
     YIELD; L+a40(0); r = poll GUI/system command (N_2ba/N_2bb/L+d0c)
     if r==1 && G[910] or r>=5: return 1          // load / title / back-to-choice
     st = N_300(win)
     if st>=3: break                              // 3 end, 4 skipped
     if st==2:                                    // waiting at a stop
        if G[846] (voice-wait mode): advance when voice ended + cfg(1,7) ms, or on click
        elif noWait: N_2fc(win,3)                 // continue without click
        elif G[844] (auto mode): timer 2 reset on entering state 2 (and while N_19);
             if N_21a(2) > cfg(1,7) && !N_2a1(5) (voice ch5 idle): N_2fc(win,3)
  if G[850]: N_29b(7); G[850]=0
  if voice && !noWait && N_9(3,9): N_29d(5)       // stop voice at line end
  G[846]=0; maybe auto-hide mouse cursor
finish:
  G[914] = N_301(win); N_c(1,1,0,0,0,0,1)          // apply pending config (FUN_0041d770)
  return 0
```
Clicks are never acknowledged by the script in normal mode: the controller
leaves state 2 by itself (§5.2). **Auto mode wait = max(cfg(1,7) ms after
the stop, end of voice)**; text length is not used.

PlayVoice:
```
slot = N_2a4(5, G[847], 6, G[775][0..5])   // §7; G[775][i] = voice-table idx of char in slot i
if slot >= -1:
   pan = 0; if slot>=0 && N_9(3,11): x=G[777][slot] (sprite x, def 400)
        pan = x<=0 ? -10000 : x<400 ? 10000*x/400-10000 : x<800 ? 10000*(x-400)/400 : 10000
   N_297(5,pan); N_29c(5,0)
   if slot>=0 && G[827][slot] && N_9(0,5): N_d1(13002); N_214(G[827][slot], N_2a3(5))   // lip sync
   if N_9(3,10) && N_292(): N_2a5(5,0)       // GUESS BGM ducking
   if N_9(3,13): G[846]=1 (voice-wait), hide window, N_2fc(win,0)
```
Sound channels: 5 voice, 7 per-line SE, 6 looping SE, 0..2 system.

Key G[] (from the lifter; V = verified usage, G = meaning guessed):
G[810] message window, G[811] name box, G[812] choice window (type 6),
G[813] #choices shown, G[814] window visible, G[816] click cursor,
G[817][0..9] frame buttons (MS_FRAME.GMP), G[818][i] button state
(0 disabled/1 enabled/2 toggled), G[819] button bar visible, G[844] auto
mode, G[846] voice-wait state, G[847] voice-id buffer (≤16 B), G[850] SE
file of the line, G[905]/906/908 restore replay (G), G[907]/G[807][]/G[808]
choice history replay, G[910] abort code (1 title, 2 loaded, 3 back to
choice), G[913] selected choice, G[914] auto-read-skip active, G[915] skip
mode, G[775]/777/827 character slot voice index / x / lip object.
Buttons / commands (`L+d0c(cmd)`): 0 quicksave, 1 quickload, 2 back to
previous choice, 3 save, 4 load, 5 skip toggle, 6 auto toggle, 7 history,
8 system menu, 9 hide window (also right click), 10 config, 11 title;
F1..F5 (+Shift/Ctrl) = save/load slot.

### 10.1 Backlog screen (history.sob)
Does not use N_1d1/N_1dc; it builds 4 rows of real windows bound to the
message manager, font G[848]:
* name box `N_2b1(139, 44+140i, 144,18, …)`, message window
  `N_2b2(139, 71+140i, 612,64, …)` with `N_2f1(win,1,0,5,1,2,0)` (line
  spacing 5), `N_2fe(win,name,1)`, row click rect y = 34+140i; scrollbar
  `N_2af(765,42,…TH_BAR/TH_UP/TH_DOWN…)`, `N_2b0(sb,536,count,4,count-4,0,8)`,
  bg `SYS/TH_BASE.ELG`, voice buttons `TH_VOICE` at (119,34+140i), face
  icons `TH_<NAME>.ELG` at (10,41+140i) picked via `N_2a4(-1,voiceId,0)`.
* refresh at scroll pos p: for i in 0..3: `N_1e3(p+i)` (load entry),
  `N_2fb(win[i])` (instant render), `N_302(win[i],buf)` (voice id →
  enables replay button). Keys: PgUp/PgDn ±4, up/down/wheel ±1, Home 0,
  End last. On exit: `N_1e2()` + `L+724(0)` re-render the current line.

### 10.2 Choices (L+68c0)
`N_1ce(question); L+2c3c(1); N_1eb(rec); …; L+68c0()` → G[913].
Menu: `N_2ea(sel)` clear; for i < N_1f1(): `N_309(sel, N_1f4(i), 0,
G[854][i]?1:-1, 0,0,0)`; show; skip/auto suspended; optional auto-save
(`N_9(0,10)`); GUI id ≥ 20 → `N_1f5(id-20)` (sets bitset2 = "chosen
before"; selection object greys/tints via N_30b/N_30c + vtbl+0xa0);
optional blink feedback (N_30a ×5, 40/80 ms) with click SE. Selection
object colours from advinit: N_306(sel, 0xfff000, 0x8fdfdf, 0xffffff)
(+0x909090), N_307(sel, 0xff9292,0xffff46,0x26dd26,65515,0xff4646,0xffeb00)
(+0xb0b0b0, 0x595959), frames SYS/SEL0{0,1,2}{G,A,B}.ELG, choice window
`N_2b3(38,220,0,0,0,G[900],1500)` at (38,220) — see graphics/UI doc.
(Window-creating natives take `(x, y, w, h, font, …, z)`; w=h=0 → size
from the frame image; e.g. history `N_2b2(139,71,612,64,G[848],…)`.)

---------------------------------------------------------------------------

## 11. Config fields used by the text system (app = DAT_00503248)

Set by N_8(cat, idx, v), read by N_9(cat, idx); defaults from the EXE
table (FUN at ore_tuba.c ~11530) and config1 "reset".

| cat,idx | field | default | meaning |
|---|---|---|---|
|0,2|+0xe45|1|skip (button) only through read text |
|0,5|+0xe48|?|voice/lip-sync related flag (voice flag 2, lip sync enable) GUESS |
|1,0|+0xe55|0|unread text: instant page |
|1,1|+0xe56|30|unread text: ms per character |
|1,2|+0xe57|1|read text: instant page |
|1,3|+0xe58|30|read text: ms per character |
|1,4|+0xe59|0|auto-skip read text (no UI) |
|1,5|+0xe5a|50|backlog size (capacity = v+1 = 51) |
|1,7|+0xe5c|1000|auto-mode wait (ms, u32) |
|2,0|+0xe60|"ＭＳ ゴシック"|face |
|2,1|+0xe80|1|AA mode |
|2,2|+0xe81|0|gradient |
|2,3|+0xe82|2|effect 0 none/1 shadow(2,2)/2 edge |
|2,4|+0xe83|0|bold |
|2,6..8|+0xe85/+0xe86/+0xe88|1 / 140 / 0|window bg mode / alpha / RGB |
|3,4|–|1|voice on |
|3,9|–|1|stop voice when advancing |
|3,11|–|0|voice panning by sprite x |
|3,13|–|0|voice-wait mode |
|4,0|+0xeb8|(dialog)|skip key: 1 Ctrl (DIK 0x1d/0x9d), 0 Shift (0x2a/0x36) |
|4,1|+0xeb9|0|skip key only through read text |

---------------------------------------------------------------------------

## 12. Reimplementation checklist

1. Parse record: type/line#, name field (`\`-split name/voice), body.
2. isRead = bitset1[script][line#]; mark read when the body end is reached
   (even when skipped). Persist per script name in TEXT_LOG.DAT layout or
   an equivalent; seed bit counts from SOB_INIT.DAT.
3. Push backlog entry (cap 51) with name/body/line; attach choices to the
   previous entry.
4. Window: margins 28/13/16/34, pads 2, 22 px MS Gothic cells (full-width
   chars fixed 22 px wide, centred; half-width use real advance),
   line pitch 28, wrap when x > boxW − 2·22 − 16 except kinsoku rules (§6),
   page full → drop remainder & wait.
5. Per frame: reveal ≤1 glyph when `now ≥ next`, `next = now + 30`
   (instant for read text by default); `<N>` changes font size for the
   message; `$` = wait with MS_CURSR animation; skip key/button/auto as
   §5.2/§10.
6. Name box at (4,427), left aligned (mode 1); message window at (4,466).
7. Voice: `VOICE\<id>.OGG` on ch5, prefix→character table from
   VOICEINF.DAT, per-character volume/enable; lip sync and pan optional.
8. Text rendering: GRAY4-like AA, white text, soft black edge radius 2
   gain 4 (dilated alpha), "over" composite into the window layer.

---------------------------------------------------------------------------

## Open questions

1. Page overflow: FUN_00409090 calls FUN_00456a90(mm, −1) which, as read,
   skips the rest of the message up to `&`/NUL (text discarded) and then
   waits; another reading (lib agent) claims a one-character rewind.
   Re-check with a test (no shipped line is expected to overflow).
2. Frame-image roles of MS_FRM_G/A/B (+0x24c/+0x248/+0x244) and how
   +0x30/+0x34 (box size) derive from them; exact window size (likely
   792×~130).
3. msgwin+0x2c4 is checked as typeId 1 (the glyph-font class also reports
   1); the code treats it as a button (state byte +0x3f, enable vtbl+0x2c).
   Confirm the GMP part class id.
4. Tick time base for the controller (N_2ba → FUN_00449820) assumed ms;
   frame rate of the task loop (one glyph per tick upper bound).
5. Ruby (`vtbl+0x114`, ruby font +0x2c) and `<"str">`, `@nnn` gaiji, `[`
   blocks, `%` interpolation, record type≠0 are implemented but unused by
   the Japanese scripts; check the Chinese scripts (SCRIPT_CHS) for any
   use before dropping them.
6. Chinese build: kinsoku tables are SJIS byte pairs; with GBK text they
   match wrong characters (or none). Decide on Unicode kinsoku in the port.
7. tmAscent of MS Gothic 22 px and whether GDI used embedded bitmaps
   (affects pixel-exact baseline/edge look).
8. Who sets G[905]/G[906]/G[908] on load / back-to-choice (probably native
   save restore), and the N_1f8/N_1fc "LAG" object format (logo only).
9. TEXT_LOG.DAT write trigger (FUN_00423e30 callers) and exact compressor
   (FUN_00495420/FUN_00495710).
