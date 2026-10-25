# Talking to `/dev/fb0` on Allwinner sunxi (disp2) Linux — the correct sequence

**Part:** Allwinner sun8iw20 (marketed T113 / R528), 2x Cortex-A7, Tina Linux
**5.4.61**, sunxi **disp2** framebuffer driver over **ION**. The same driver, with
the same custom ioctls, ships in the other sunxi BSPs of this generation, so the
sequence below applies beyond this one part.
**Reference device:** a 532x300, 32 bpp panel on which every number below was
measured. Everything here comes from the driver source and the device's own
stock GUI binary, not from documentation; every number has a named source.

## 0. Where this comes from

- **Driver source:** `drivers/video/fbdev/sunxi/disp2/disp/dev_fb.c`, in two
  public BSP trees whose custom-ioctl block and handlers are identical:
  - sun8iw20 / T113, linux-4.9: `https://github.com/YuzukiHD/TinyVision`
    (`kernel/linux-4.9/drivers/video/fbdev/sunxi/disp2/disp/dev_fb.c`).
    Line numbers below refer to this file.
  - sun50iw9 / H616, linux-5.4: `https://github.com/orangepi-xunlong/linux-orangepi`,
    branch `orange-pi-5.4-sun50iw9`, same path.
- **The device's stock GUI** (an LVGL application, shipped unstripped with
  symbols): its `sunxifb_init` / `sunxifb_flush` / `sunxifb_exit` are the
  reference implementation that is known to light the panel. Disassembled call
  order and immediates are quoted where they settle a point.
- **Live measurements** (read-only probes on the reference device): the geometry
  and the touch facts in section 1.
- **Caveat:** no public sun8iw20 **5.4** tree was found. The ioctl numbering is
  identical across the SoC axis (sun8iw20 / 4.9) and the kernel-version axis
  (sun50iw9 / 5.4) and matches the GUI binary, so it is taken as confirmed for
  this kernel. Anything that a future 5.4 tree contradicts is flagged in
  section 9.

## 1. The reference device, measured

| Fact | Value | Source |
|---|---|---|
| Node | `/dev/fb0` | live |
| Visible | **532 x 300** | `FBIOGET_VSCREENINFO` live |
| Virtual | **532 x 600** — two pages | `yres_virtual` live |
| Depth | **32 bpp**, `red 16/8`, `green 8/8`, `blue 0/8`, **`transp 24/8`** | var bitfields; driver `disp_fb_to_var` ARGB_8888 case |
| Stride | **2128** bytes (= 532 x 4) | `FBIOGET_FSCREENINFO` live |
| Mapped length | **1,276,800** bytes (both pages) | `smem_len` live |
| Memory order | `B, G, R, A` (little-endian word `0xAARRGGBB`) | bitfields |
| Page offsets | page 0 at byte 0, page 1 at **638,400** (300 x 2128) | derived |
| Touch | `/dev/input/event0`, `sitronix_ts_i2c`, EV_ABS, X[0..531] Y[0..299], rotate 0 | live |
| Stock GUI | `/opt/bin/ec-eeb001-gui`, launched bare, **no respawn**, **ignores SIGTERM** (`killall -9`), restart `ec-eeb001-gui > /dev/null 2>&1 &` | live |

Another sunxi device will differ in the panel numbers and the GUI; read them
with the same ioctls and the same `ps`, and the rest of this document holds.

## 2. The memory model — what the driver actually does

1. **One ION buffer for the whole fb.** `fb_map_video_memory` (102-131) calls
   `disp_ion_malloc(smem_len)` once; `screen_base` is the kernel mapping of that
   buffer; it is memset to black at init. Both pages live in it.
2. **The display engine scans PHYSICAL RAM by DMA.** It never sees a CPU cache.
3. **Whether YOUR mapping is cached is decided when you call `mmap`, from a
   kernel-global per-node flag** (`sunxi_fb_mmap`, 839-862):
   `if (g_fbi.mem_cache_flag[node])` the dma-buf is flagged
   `ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC` and mapped **cached**;
   otherwise flagged 0 and mapped **uncached**. The flag lives in a `static`
   struct (58) and is never initialised, so its **boot default is 0 =
   uncached**.
4. **`FBIO_ENABLE_CACHE` does nothing except set that flag** (1359-1365,
   `mem_cache_flag[node] = (karg[0] == 1) ? 1 : 0`). It does not touch an
   existing mapping. It therefore only has an effect if it runs **before**
   `mmap` — and **the flag persists across processes**: whatever the last
   process set is what the next `mmap` gets.
5. **`FBIO_CACHE_SYNC` cleans the whole buffer** (1367-1373):
   `disp_ion_flush_cache(mem[node]->vaddr, mem[node]->size)` through the kernel
   mapping, under `CONFIG_ION_SUNXI`. The third ioctl arg is **ignored**.
   Cortex-A7 caches behave as PIPT for coherency, so dirty lines written through
   a user mapping of the same pages are cleaned regardless of the virtual alias.
6. **`FBIOPAN_DISPLAY` reprograms the layer's source crop and waits for vsync**
   (`sunxi_fb_pan_display`, 644-757): `crop.x/y = xoffset/yoffset`,
   `crop.width/height = xres/yres`, `set_layer_config`, then
   `fb_wait_for_vsync`. It does **not** flush any cache.

**What this means:** with an uncached mapping, plain writes reach RAM and the
panel shows them after a pan with no flush — but every READ of the fb is
uncached and slow. With a cached mapping, writes sit in cache until they are
cleaned; the panel shows nothing new until `FBIO_CACHE_SYNC` runs. The stock
GUI chooses **cached + flush** and so should any client that reads the fb
(page seeding, CopyRect).

**The trap:** the GUI sets the flag to 1 and, if it is killed with `-9`, its
`sunxifb_exit` (which resets the flag to 0) never runs. The next process to
`mmap` — without asking — gets a **cached** mapping and, if it never flushes,
a black panel. **Never rely on the inherited flag. Set the mode yourself,
before `mmap`, every time.**

## 3. The pixel format — the alpha byte is live

The var advertises `transp.offset 24, length 8`, and the driver maps 32 bpp to
`DISP_FORMAT_ARGB_8888` (`sunxi_fb_check_var`, 771-777). **The fb layer is
created with `alpha_mode = 0`, i.e. PER-PIXEL alpha** (`display_fb_request`,
~2288-2291: `mode = LAYER_MODE_BUFFER; zorder = 16; alpha_mode = 0;
alpha_value = 0xff` — `alpha_value` is only used in modes 1 and 2).

**So the display engine multiplies every pixel by its top byte.** A pixel with
`A = 0x00` is invisible, whatever its RGB. The stock GUI (LVGL, 32-bit colour)
writes `0xFF`; an RFB client copying a server's bytes gets whatever the server
put in a byte the protocol calls padding — usually `0x00`.

**Rule: every 32-bpp write to this fb sets the alpha byte to 0xFF.** A clear
is `0xFF000000` (opaque black), not `memset 0` (transparent black, which lets
anything below zorder 16 show through).

## 4. The ioctls

| ioctl | Number | Arg | What the kernel does | Source |
|---|---|---|---|---|
| `FBIOGET_VSCREENINFO` | `0x4600` | `struct fb_var_screeninfo *` | geometry, bitfields, current `xoffset/yoffset` | standard; GUI `sunxifb_init` |
| `FBIOGET_FSCREENINFO` | `0x4602` | `struct fb_fix_screeninfo *` | `line_length`, `smem_len` | standard; GUI `sunxifb_init` |
| `FBIOPAN_DISPLAY` | `0x4606` | `struct fb_var_screeninfo *` (set `xoffset`, `yoffset`) | layer crop to the page; **blocks for vsync** | standard; driver 644-757; GUI `sunxifb_flush` (`movw r1,#0x4606`) |
| **`FBIO_CACHE_SYNC`** | **`0x4630`** | any (ignored) — pass `unsigned long[2] = {0, smem_len}` | clean the WHOLE buffer to RAM | driver 1104, 1367-1373; GUI `sunxifb_flush` (`movw r1,#0x4630`) |
| **`FBIO_ENABLE_CACHE`** | **`0x4631`** | `unsigned long[2]`, word 0 = **1 enable / 0 disable** | sets `mem_cache_flag[node]`; consumed by the NEXT `mmap` | driver 1105, 1359-1365; GUI `sunxifb_init` (`{1,0}`) and `sunxifb_exit` (`{0,0}`) |
| `FBIO_GET_IONFD` | `0x4632` | — | dma-buf fd of the allocation | driver 1106 — **not a cache op; do not use** |
| `FBIO_GET_PHY_ADDR` | `0x4633` | — | physical address | driver 1107 — **not needed; do not use** |

**There is no `FBIO_DISABLE_CACHE`.** Disable is `FBIO_ENABLE_CACHE` with word 0
= `0`. **`0x4632` is not the flush** — a client that used it would call
`GET_IONFD` and never clean the cache.

A standard toolchain's `linux/fb.h` does not define the sunxi numbers; define
them in the client:

```c
#define FBIO_CACHE_SYNC     0x4630              // clean the whole fb to RAM
#define FBIO_ENABLE_CACHE   0x4631              // word 0: 1 = cached mmap, 0 = uncached
```

## 5. The correct lifecycle and per-frame sequence

```
OPEN  (once)
  1. open("/dev/fb0", O_RDWR)
  2. FBIOGET_VSCREENINFO   -> xres, yres, yres_virtual, bitfields (incl. transp), yoffset
  3. FBIOGET_FSCREENINFO   -> line_length, smem_len
  4. FBIO_ENABLE_CACHE {1, 0}   *** BEFORE mmap ***   (best effort: log a -1, continue)
  5. mmap(NULL, smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)   -> both pages
  6. front page = (yoffset >= yres) ? 1 : 0 ; back page = the other

FRAME (per presented frame)
  7. seed the back page from the front page (fb -> fb memcpy; cached, so fast)
  8. write pixels into the back page, alpha byte forced to 0xFF
  9. FBIO_CACHE_SYNC {0, smem_len}   *** BEFORE the pan ***
 10. FBIOPAN_DISPLAY  with xoffset = 0, yoffset = back * yres,
                      reserved[0..3] = {0, 0, xres, yres}, activate = FB_ACTIVATE_NOW
                      (blocks until vsync; check the return value)
 11. swap: back <-> front

CLOSE (once)
 12. FBIO_ENABLE_CACHE {0, 0}     (restore the boot default for whoever maps next)
 13. munmap ; close
 14. only now relaunch the stock GUI
```

This is the GUI's own order (`open` -> `0x4602` -> `0x4600` -> `0x4631{1}` ->
`mmap` ... per flush: `memcpy` -> `0x4630` -> `0x4606` ... exit: `0x4631{0}`),
with two additions a copying client needs: the alpha forcing (section 3) and
the `reserved[]` fill (section 6).

## 6. Page flipping — what the pan needs

- The two pages are the two halves of `smem_len`; the visible one is selected by
  `yoffset` (0 or `yres`). The kernel programs `crop.y = yoffset`,
  `crop.height = yres` — a clean flip, no copy.
- **The pan waits for vsync** (`need_wait_vsync = 1`, `fb_wait_for_vsync`), so it
  costs up to one frame period and cannot tear. Do not add a second wait.
- **`reserved[0..3]` trap:** under `CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT`, if a
  software-rotation layer (`mgr->rot_sw`) is active and `reserved[2]` or
  `reserved[3]` is 0, the pan **returns 0 without panning** (682-690). A
  rotate-0 panel almost certainly has `rot_sw` NULL — but a var copied from
  `FBIOGET_VSCREENINFO` carries zeros there. Always set
  `reserved = {0, 0, xres, yres}` before the pan: free when it does not matter,
  correct when it does.
- One `FBIO_CACHE_SYNC` before the pan is enough for both pages (it cleans the
  whole buffer), so seed-copy, blits and CopyRects in the back page are all
  covered by the single flush.

## 7. Rules

1. **One mapper at a time.** `ENABLE_CACHE` at `mmap` rewrites the dma-buf's own
   flag. Stop whatever process owns the display before opening (on the
   reference device `killall -9 ec-eeb001-gui`, since it ignores SIGTERM); unmap
   and close before relaunching it. `open()` will NOT fail while another process
   holds the fb — fbdev has no exclusive lock — so the ordering is a discipline,
   not something the kernel enforces.
2. **Set the cache mode before `mmap`, every open.** Never inherit it.
3. **Cached + flush is the mode for a client that reads the fb** (page seeding,
   CopyRect). Uncached is only right for a write-only client with its own
   shadow buffer.
4. **Force alpha 0xFF on every write; clear to opaque black.**
5. **Flush before every pan; pan after every flush.** Never pan a page that has
   not been cleaned since it was last written.
6. **Check every ioctl's return and log `errno`** at least once — a silent `-1`
   from `ENABLE_CACHE`, `CACHE_SYNC` or `PAN` is exactly how a black panel
   becomes a mystery.
7. **Do not use `FBIO_GET_IONFD`, `FBIO_GET_PHY_ADDR`, the `/dev/g2d` blitter or
   the `FBIOGET_LAYER_HDL` family.** The stock GUI uses G2D for rotation/blits;
   a pixel-copy client needs none of it.
8. **Do not change the mode** (`FBIOPUT_VSCREENINFO`): the geometry and format
   are read, never written.

## 8. Diagnostics that settle each step on the box

| Question | How to answer it without guessing |
|---|---|
| Is the mapping cached? | After `mmap`, `grep fb0 /proc/<pid>/maps` shows the VMA; the flag itself is only visible as behaviour — write a pattern, pan WITHOUT `CACHE_SYNC`: cached shows nothing, uncached shows it |
| Did the flush run? | log the `CACHE_SYNC` return (0) and `errno`; `-1 ENOTTY` means the number is wrong or ION is absent |
| Did the pan take? | log the `PAN` return; then `FBIOGET_VSCREENINFO` again — `yoffset` must equal what was requested |
| Is alpha the problem? | write a full-screen `0xFFFF0000` (opaque red) vs `0x00FF0000`: only the first shows under pixel alpha |
| Which page is on screen? | `FBIOGET_VSCREENINFO`'s `yoffset` |
| Is the previous owner really dead? | `ps w` (busybox has no `pgrep -x`); `pgrep -f` can match a transient `ash` wrapper |
| Is the panel dark because the backlight is off? | `cat /sys/class/disp/disp/attr/sys` and read the `backlight(N)` figure on the `lcd output` line (section 11); 0 means off, whatever the framebuffer holds |

## 9. Open items and caveats

- **No public sun8iw20 linux-5.4 tree** was located; the numbering is confirmed
  by two-axis agreement plus the device's own GUI binary (section 0).
- **`FBIO_CACHE_SYNC` arg:** ignored on the sun8iw20/4.9 handler; the GUI passes
  a 2-word `{offset, size}` anyway. Passing `{0, smem_len}` satisfies both a
  handler that ignores it and one that honours it.
- **`rot_sw`:** not expected on a rotate-0 panel; the `reserved[]` fill covers
  the case regardless.
- **`CONFIG_ION_SUNXI`:** the cache handlers are compiled only with it. The GUI's
  `SunxiMemFlushCache` / `/dev/g2d` use shows ION is present on this firmware.

## 10. Minimal annotated C (the sequence, not a module)

```c
/* Open: read geometry, choose the cache mode BEFORE mapping. */
fd = open("/dev/fb0", O_RDWR);
ioctl(fd, FBIOGET_VSCREENINFO, &var);           /* e.g. xres 532, yres 300, yres_virtual 600 */
ioctl(fd, FBIOGET_FSCREENINFO, &fix);           /* e.g. line_length 2128, smem_len 1276800 */
au[0] = 1; au[1] = 0;
rc = ioctl(fd, FBIO_ENABLE_CACHE, au);          /* cached mapping from here on; log rc */
mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
back = (var.yoffset >= var.yres) ? 0 : 1;       /* draw into the page NOT on screen */

/* Frame: seed, draw with alpha, clean, flip. */
memcpy(mem + off[back], mem + off[1 - back], var.yres * fix.line_length);
/* ... write pixels into mem + off[back], OR-ing 0xFF000000 into every word ... */
region[0] = 0; region[1] = fix.smem_len;
rc = ioctl(fd, FBIO_CACHE_SYNC, region);        /* whole-buffer clean; log rc */
var.xoffset = 0; var.yoffset = back * var.yres;
var.reserved[0] = 0; var.reserved[1] = 0; var.reserved[2] = var.xres; var.reserved[3] = var.yres;
var.activate = FB_ACTIVATE_NOW;
rc = ioctl(fd, FBIOPAN_DISPLAY, &var);          /* blocks for vsync; log rc */
back = 1 - back;

/* Close: restore the default mode, then release; only then relaunch the GUI. */
au[0] = 0; au[1] = 0;
ioctl(fd, FBIO_ENABLE_CACHE, au);
munmap(mem, fix.smem_len);
close(fd);
```

## 11. Brightness, backlight and blanking — not fbdev's business here

On this driver the backlight belongs to the display-engine driver, not to fbdev
and not to a `/sys/class/backlight` device (that class is empty on the reference
device). The controls, read from the device's own GUI binary and the driver's
sysfs, the set and get ioctls since exercised live by the reference client:

- **Brightness is set and read through `/dev/disp`** (character device 244,0,
  root only). Set: `ioctl(fd, 0x102 /* DISP_LCD_SET_BRIGHTNESS */, arg)` with
  `unsigned long arg[4] = { screen, brightness, 0, 0 }`, screen 0 and brightness
  0..255. Get: `ioctl(fd, 0x103 /* DISP_LCD_GET_BRIGHTNESS */, arg)` with
  `arg[0] = screen`, the value coming back as the ioctl's return value. The
  GUI's "screen off" is brightness 0 through the same set call. The layout is
  read from the GUI's `screen_brightness_set`: `add r2, sp, #4` (the array),
  `movw r1, #0x102`, `stmib sp, {r4, r5}` storing `{0, value}` into it.
- **The disp2 ioctl table also carries `0x104` / `0x105`,
  `DISP_LCD_BACKLIGHT_ENABLE` / `DISP_LCD_BACKLIGHT_DISABLE`**, the pair that
  sits beside the brightness calls in every sunxi disp2 tree; the GUI does not
  use them and they were not tried here.
- **The current value is visible read-only** in `/sys/class/disp/disp/attr/sys`:
  the `lcd output` line reads `backlight(175)` on the reference device at its
  default setting.
- **`/sys/class/pwm/pwmchip0` is the SoC's `2000c00.pwm` (8 channels) with
  `pwm0` exported, but its sysfs values do not track the backlight** - they
  read the same with the panel lit and dark (below), so the driver's backlight
  path is not observable there. Leave it alone; the ioctls above are the
  interface and the disp attribute is the indicator.
- **`/sys/class/graphics/fb0/blank`** exists and is writable (the fbdev blank
  hook; the GUI opens a "screen blank device" as well), but reading it returns
  nothing, so the disp attribute above is the reliable statement of the state.

**The two states, measured on the reference device**: dark after its GUI's
idle timeout (about thirty minutes, no glow at all), then lit again after a
tap woke it:

| Item | Panel lit | Panel dark |
|---|---|---|
| `lcd output` line in `/sys/class/disp/disp/attr/sys` | `backlight(175)` | `backlight(  0)` |
| `mgr0` state in the same attribute | `unblank` | `unblank` - the display pipeline keeps running, the layer stays enabled at pixel alpha 255 |
| `pwm0/enable`, `duty_cycle`, `period`, `polarity` | `0`, `166666`, `333333`, `normal` | **identical** - this sysfs channel does not reflect the backlight |
| `/sys/class/graphics/fb0/blank` | reads empty | reads empty - fbdev blanking is not used |
| framebuffer contents (visible page, 159 600 pixels) | the GUI's frame, every pixel non-black | **every pixel opaque black** |
| kernel log | - | nothing new |

So the only observable of the backlight is the disp attribute, and the GUI
clears its frame to black before going dark and redraws it on waking. **The
backlight value survives the GUI being killed**: a client that takes over a
panel the GUI had already turned off draws into a black, unlit screen, and
nothing it writes to the framebuffer can be seen until brightness is set again
through `/dev/disp`.

A framebuffer client that wants to dim, darken or wake the panel therefore does
it with one ioctl on `/dev/disp`, not through the framebuffer at all; a black
framebuffer and a backlight at 0 look the same from the front and are told
apart with the diagnostic row in section 8.

**What a client that takes the panel over should do**, as the reference
client does:

1. At takeover, read the brightness (`0x103`); if it is 0, set a usable level
   (`0x102`; the reference device's factory level is 175), otherwise leave it.
2. At exit, put back a lit level it changed, but never hand back 0: a panel
   found dark and lit stays lit, because the next owner may not light it.
3. Optionally, carry the remote desktop's own blanking to the panel: when every
   visible pixel of a presented frame has been black for a timeout (a minute is
   a reasonable default), set brightness 0; on the first presented frame with
   any colour, and at exit, set the lit level back. The link carries only
   pixels, so the black frame is the only signal a plain RFB server gives.
   Touch still reaches the server while the panel is dark, which is what wakes
   the desktop.
4. Expose the same set call as a one-shot command, so a host that knows the
   desktop's state can drive the panel over ssh beside the running client.
