# PrimeVNC - framebuffer VNC client for Allwinner sunxi Linux devices

PrimeVNC is a small, dependency-free RFB (VNC) **client** for embedded Linux
devices whose display is a bare framebuffer: it connects to a VNC server on
another machine, draws the remote framebuffer to `/dev/fb0`, and forwards the
device's touchscreen (`/dev/input/eventN`) back as RFB PointerEvents. It is a
pure sink/source: pixels in, touch out. Pure C99, no external libraries, one
static ELF. The only hardware-specific code is `framebuffer.c` (the Allwinner
sunxi disp2 driver's cache and alpha rules, written up in
`SUNXI_FB_INTERFACE.md`) and `input.c` (evdev), so it adapts easily to another
fbdev platform. Everything - target, port, password, devices, rotation, the
reconnect policy - is given on the command line.

**Reference device.** The client was developed on an Allwinner sun8iw20
(T113 / R528) device: two Cortex-A7 cores, ARMv7-A hard-float, a Linux 5.4
kernel with the sunxi disp2 framebuffer driver, and a 532x300, 32 bpp touch
panel. That panel is the reference device throughout this tree. In its
original deployment the VNC server exports a touch-driven user interface from
a host on the same LAN, and the client turns the device's own panel into that
interface's screen while the host is running.

**Status: proven on the reference device in two live sessions.** The first
proved the protocol, exact pixels in framebuffer memory and the touch
round-trip, and exposed the two framebuffer facts that kept the panel black;
the second, with those fixed, showed animated content moving smoothly on the
panel with taps landing where touched. Nothing on the device was modified or
flashed: the client ran from a temporary directory, and the program that owns
the display was only stopped and relaunched by the wrapper around it.

---

## Current state (what is done)

- **Complete C client**, pure C99, **zero external libraries** (rolled-own DES and
  RFB), single-threaded `poll()` control loop.
- **Builds clean** (0 warnings, `-Wall -Wextra -Wshadow -Wpointer-arith`) for:
  - **`armhf`** - the deployment target: `arm-linux-gnueabihf-gcc` 13.3.0,
    `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -static`. Verified:
    `ELF 32-bit ARM EABI5, statically linked`; `Tag_CPU_arch: v7`, `Tag_FP_arch:
    VFPv4`, `Tag_ABI_VFP_args: VFP registers`; **no `NEEDED` entries** (fully
    static) - exactly the reference device's ABI.
  - **`native`** - x86-64, for developing the RFB core off-hardware.
- **The DES engine passes its known-answer tests** (the textbook FIPS-46
  vector and the VNC key-wrapper check).
- **The protocol core is verified end to end** against a VNC server serving a
  known pixel pattern: version 3.8 handshake, VNC authentication, pixel-format
  negotiation, a full RAW frame decoded to exact pixels, a pointer round-trip,
  and the heartbeat request answered.
- **Proven live on the reference device:** the static binary runs on it, opens
  `/dev/fb0`, autodetects the touch panel, connects over the LAN, authenticates,
  negotiates RFB 3.8, decodes RAW frames to EXACT pixels in framebuffer memory,
  and touch round-trips to the server at the right coordinates.
- **The panel stayed black in the first live session**, for two independent
  reasons found by a driver-side review of every file, both fixed in
  `framebuffer.c`: the fb mapping's cache mode is decided at `mmap` from a
  kernel flag the killed display owner left set (so writes never reached
  RAM), and the fb layer is per-pixel alpha while every pixel written carried
  alpha 0x00. The correct way to drive this framebuffer is written up once,
  in `SUNXI_FB_INTERFACE.md`.

Static linking is safe here because the client does **no name resolution** (it is
handed a numeric IP), so glibc's NSS static-link hazard never applies.

---

## Layout

| Path | What it is |
|---|---|
| `README.md` (+ `.html`) | this file |
| `LICENSE` | GPL-2.0-or-later, Copyright (c) 2026 B. C. Services |
| `GPL-2.0.txt` | the licence's full text, verbatim from the FSF |
| `BUILDING.md` (+ `.html`) | build guide: tools with download links, targets, checks |
| `SUNXI_FB_INTERFACE.md` (+ `.html`) | the correct `/dev/fb0` sequence on this part (reference) |
| `build.sh` | build script: `armhf` \| `native` |
| `primevnc-run.sh` | sample launcher: stop the stock UI and run the client |
| `primevnc-restore-ui.sh` | sample: restart the stock UI when the session ends |
| `.gitignore` | excludes `build/` (binaries are not committed) |
| `src/primevnc.h` | shared config struct, TRUE/FALSE/SUCCESS/FAILURE, version |
| `src/log.{h,c}` | error-only stderr logging (+ `--verbose` diagnostics) |
| `src/net.{h,c}` | numeric-IPv4 TCP (no DNS), bounded connect, deadline reads |
| `src/des.{h,c}` | single-block DES + VNC key bit-reversal (RFC 6143 7.2.2) |
| `src/pixfmt.{h,c}` | pixel-format descriptor + fast-path test + converter |
| `src/framebuffer.{h,c}` | `/dev/fb0`: cache mode, alpha forcing, page flip, blit, CopyRect, backlight |
| `src/input.{h,c}` | evdev touch (single + multitouch B), ABS->screen mapping |
| `src/rfb.{h,c}` | RFB 3.8 client: handshake, auth, updates, heartbeat, pointer |
| `src/main.c` | CLI, control loop, liveness rule, reconnect, exit codes |
| `build/` | build outputs (git-ignored) |

---

## Building

**`BUILDING.md` is the build guide**: the toolchains with download links, every
target, the flags and how to adapt them, the expected verification output, and
a troubleshooting table. In short, in a Linux environment with the compilers
installed, from this directory:

```bash
bash build.sh            # static armhf ARMv7 -> build/primevnc-armhf   (default)
bash build.sh native     # x86-64 host build  -> build/primevnc-native
```

The armhf build runs `file` and `readelf -A` / `-d` afterwards to confirm the ABI
and that the binary is static.

### Toolchain

- **Required:** `arm-linux-gnueabihf-gcc` (13.3.0 tested) with a static C
  library, and its binutils (`readelf`); native `gcc` for the host build.
- **Optional, not needed to build:** `qemu-user-static`, to run the armhf
  binary on the build machine (the framebuffer and touch devices do not exist
  under user-mode emulation, so a full run needs the device); an
  `arm-linux-musleabihf` toolchain for a smaller fully-static binary (glibc
  static is the tested configuration).

---

## Command line

Synopsis: `primevnc <ip> [options]`

| Option | What it does |
|---|---|
| `<ip>` | numeric IPv4 of the VNC server (no DNS) |
| `--port N` | RFB port (default 5900) |
| `--fb PATH` | framebuffer device (default `/dev/fb0`) |
| `--input PATH` | evdev touch node (default: autodetect) |
| `--password P` | VNC password on the command line (visible in `ps`) |
| `--password-env NAME` | read the VNC password from env var `NAME` |
| `--password-fd N` | read the VNC password from file descriptor `N` |
| `--rotate 0\|90\|180\|270` | rotate touch mapping to the panel (default 0) |
| `--reconnect-max N` | reconnect attempts; -1 = forever (default 10) |
| `--reconnect-delay MS` | base reconnect backoff (default 1000) |
| `--backlight on\|off\|keep\|N` | panel brightness at takeover (default on): on = light it if dark, off = dark, keep = leave it, N = level 1..255; a lit level this program changed is restored at exit |
| `--backlight-auto off\|on\|S` | turn the panel off after S seconds of an all-black frame (on = 60) and back on at the first lit frame (default off) |
| `--backlight-set on\|off\|N` | set the brightness and exit at once - no server, no session; for a host driving the panel over ssh while a client runs |
| `-v, --verbose` | diagnostic logging to stderr |
| `-h, --help` | this help |
| `--version` | print version and exit |

This is the full set of options the client accepts (the same options as
`primevnc --help`). The password forms and the three `--backlight*` options are
described further below.

### The backlight

The panel's brightness is not the framebuffer's: on the sunxi disp2 driver it
is set through `/dev/disp`, and the value survives whatever process last owned
the display. The reference device's stock user interface turns the panel off
after about thirty idle minutes (brightness 0, frame cleared to black), so a
client that takes over such a panel would draw into an unlit screen. Three
controls, all in `framebuffer.c` (`SUNXI_FB_INTERFACE.md` section 11 has the
driver facts):

- **`--backlight`** is the takeover policy. `on` (the default) reads the
  brightness and lights a dark panel at the reference device's factory level,
  leaving a lit one at its own level; `off` and a level exist for tests; `keep`
  touches nothing. At exit the client restores a lit level it changed, and
  never hands back a dark panel: one it found dark and lit stays lit.
- **`--backlight-auto`** carries a remote desktop's own screen blanking to the
  panel over a link that carries only pixels: when every pixel of a presented
  frame has been black for the timeout, the panel goes dark; the first frame
  with any colour brings it back to the level lit at takeover, and so does
  exit. A tap on the dark panel still reaches the server, which is what wakes
  a sleeping desktop. The check runs once per control-loop pass, so the panel
  darkens within one idle poll period (8 s) of the timeout.
- **`--backlight-set`** is a one-shot for the host: it sets the brightness and
  exits without opening the framebuffer, the touch device or a socket, so it
  can run over ssh beside a live client whenever the host knows better than
  the black-frame rule that the desktop went to sleep or woke.

The password may be given bare with `--password`, or through an env var or an fd
for a launcher that prefers to keep it out of `ps`. The link is plain RFB with
VNC authentication and no transport encryption, which suits a trusted LAN and
nothing else; how the deployment chooses and stores the password is the
launcher's business, not the client's.

---

## How a launcher drives it (the reference deployment's lifecycle)

The device boots to its own stock user interface - a bare framebuffer
application, launched by the device's init with no respawn - and is fully
usable standalone. The remote interface is an optional, transient overlay that
exists only while the host is running. When the host enables it, at startup
it:

1. determines its own IP as seen by the device (the SSH connection's source
   address);
2. logs in over SSH as root and stops the program that owns the display
   (`killall -9 <its name>`; on the reference device that program ignores
   SIGTERM) to free `/dev/fb0` and the input device;
3. copies this binary to a writable location on the device if it is missing or
   stale, and makes it executable;
4. starts its VNC server locally, then launches the client backgrounded
   through a wrapper that relaunches the display owner whenever the client
   exits, for any exit code: `primevnc <host-ip> --password ... ; <relaunch
   the display owner>`. The relaunch must give that program the environment
   the device's init gives it - its library path, its `PATH`, its `HOME` -
   with stdio on `/dev/null` and backgrounded; a bare relaunch from an ssh
   command shell can fail silently because that shell lacks those variables.

On a **clean stop** the host runs `killall primevnc` and the wrapper relaunches
the display owner. On an **unclean loss** the client detects it and exits after
its bounded reconnect backoff, and the same wrapper brings the display owner
back - so the device returns to a usable interface with no cleanup from the
host. **The client itself knows nothing about that program**: it never stops or
starts another process. It exits with a distinct code (2 no framebuffer, 3 no
touch device, 1 usage error, 0 after a clean stop or exhausted reconnects) and
the wrapper does the rest, so a failed start never leaves the panel dead
either.

How a lost server is detected: a connect attempt is bounded at 5 s (a dropped SYN
does not park the client for minutes); every read has one deadline for the
whole message; and when the link is idle for 8 s the client sends a heartbeat -
a non-incremental 1x1 update request, which a server MUST answer - and gives the
session up after three heartbeats in a row go unanswered (about 32 s), which
catches a host that vanished without a FIN/RST. Reconnects that follow an
established session start a fresh attempt count, so a kiosk survives any number
of host restarts.

### Sample launcher scripts

Two reference scripts ship beside the binary, codifying the launch procedure
used to test the client on the device. Each is a SAMPLE for the reference
device -- they name the stock UI `ec-eeb001-gui` and its `/opt` paths, so adapt
them for another target -- and the client itself never stops or starts the UI;
that is entirely these scripts' job.

- **`primevnc-run.sh <ip> <password> [--background] [options]`** stops the stock
  UI (`killall -9`) and runs the client. You supply the IP and password on the
  script's own command line and they are forwarded to the client (the password
  through the environment, so it never appears in `ps`); any extra arguments
  pass straight through to `primevnc`. By default the client runs in the
  **foreground**, so the script is the session. **`--background`** instead
  detaches it -- a HUP-ignoring background subshell, because the device has no
  `nohup` or `setsid` -- so it keeps running after you log out over SSH, and it
  prints the client's pid (`kill <pid>` or `killall primevnc` stops it). That is
  a runtime detach only: it installs nothing, so it does **not** survive a
  reboot -- after a reboot the device simply comes up running its own stock UI.
- **`primevnc-restore-ui.sh`** restarts the stock UI once the session ends. It
  reconstructs the environment the device's init gives the UI -- its `PATH` and,
  critically, its `LD_LIBRARY_PATH`, without which a relaunch fails silently --
  by preferring a running daemon's own environment and falling back to the
  device's known paths.

Make them executable once on the device
(`chmod +x primevnc-run.sh primevnc-restore-ui.sh`).

**Not autorun:** nothing is added to the device's init.

---

## Development status

| Piece | State |
|---|---|
| Live probes of the reference device: fb geometry and format, touch axes and rotation, the present model | **done** (facts below) |
| RFB core: connect, VNC authentication, RAW decode, pointer, heartbeat | **done**, verified end to end against a test server |
| fbdev and evdev integration | **done**, exercised on the device |
| Static armhf cross build with `readelf` checks | **done** |
| Live deployment on the reference device with the display owner stopped | **done**, twice: protocol, pixels in memory and touch; then the panel itself, with animated content and taps |
| Host-side integration: IP discovery, launch, cleanup, display hand-back, sleep and wake forwarded to the panel | the launcher's job, outside this tree (the section above is its contract) |

---

## Confirmed facts (reference device)

Everything below was measured on the device or read from the driver source;
`SUNXI_FB_INTERFACE.md` carries the sources line by line.

- **Framebuffer:** `/dev/fb0`, 532x300 visible, 532x600 virtual (two pages),
  32 bpp, memory order B,G,R,A with a LIVE alpha byte (`transp 24/8`, per-pixel
  alpha layer). The client requests exactly this format from the server, forces
  alpha 0xFF on every pixel it writes, draws into the back page and presents
  with `FBIOPAN_DISPLAY`.
- **Cache mode:** the sunxi driver decides whether a mapping is cached from a
  kernel flag at `mmap` time. The client sets `FBIO_ENABLE_CACHE {1,0}` BEFORE
  `mmap`, runs `FBIO_CACHE_SYNC` before every pan, and resets the flag at close.
- **Touch:** `/dev/input/event0`, driver `sitronix_ts_i2c`, absolute axes
  X[0..531] Y[0..299] (1:1 with the panel), rotation 0. Autodetect prefers a
  device that declares a touch signature (`INPUT_PROP_DIRECT` or `BTN_TOUCH`)
  and logs the driver's name.
- **Display owner stop and relaunch (outside the client):** `killall -9` of
  the stock interface before; after, from `/`, the same program started with
  the environment the device's init gives it (its library path, a `PATH` that
  includes its own directory, its `HOME`), stdio on `/dev/null`, backgrounded.
  A bare relaunch fails from an ssh command shell, whose `PATH` and
  environment lack what the program needs to load its libraries. The client
  has no knowledge of either step; the launcher's wrapper does both.
- **Link:** RAW at 532x300 over a wired LAN is comfortable; add Zlib (a static
  zlib) only if a link ever proves bandwidth-bound.
- **Backlight:** brightness 0..255 through `/dev/disp` ioctl `0x102` (set) and
  `0x103` (get), argument `{screen 0, value, 0, 0}`; the stock interface's
  idle-off is brightness 0 plus a black frame, the factory level is 175, and
  the value is visible read-only as `backlight(N)` in
  `/sys/class/disp/disp/attr/sys`. The three `--backlight*` options above
  drive it - all three proven on the device: the one-shot took the panel to 0
  and back to 175 under a running client; `--backlight-auto 20` darkened the
  panel 20 s into a black screen and re-lit it on the first lit frame after a
  server restart; the driver's reading, the client's log and the panel agreed
  at every step.

Building and deploying the binary are `BUILDING.md`'s subject; every step that
touches a device is the deployment's own decision.

---

PrimeVNC is free software under the GNU General Public License, version 2 or
later: `LICENSE` beside this file carries the terms and `GPL-2.0.txt` the full
text, and every source file names them in its header.

Static analysis, host builds, and live sessions on the reference device;
nothing on the device was modified or flashed.
