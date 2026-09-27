# Building PrimeVNC

PrimeVNC is a framebuffer VNC client for embedded Linux devices: pure C99, no
libraries beyond the C library and the Linux kernel's own headers, one
statically linked executable. This document says what is needed to build it,
where to get those tools, how to run the build, and how to check the result.
The command line, the launcher lifecycle and the framebuffer driver rules are
in `README.md` and `SUNXI_FB_INTERFACE.md`; this document covers the build
only.

---

## 1. What the build produces

`build.sh` builds one of two targets. The first is the deployment binary;
the second exists for development on the build machine.

| Target | Command | Output | What it is for |
|---|---|---|---|
| `armhf` (default) | `bash build.sh` or `bash build.sh armhf` | `build/primevnc-armhf` | The device binary: ARMv7-A, hard-float, NEON, **statically linked**. Copy this to the device. |
| `native` | `bash build.sh native` | `build/primevnc-native` | The same client compiled for the build machine, for developing the protocol core. It still needs a framebuffer and an evdev device to run. |

Every source file is named explicitly in `build.sh`; there are no wildcards.
Removing a listed file fails the build, and a new file is not part of the
program until it is listed. That is deliberate.

The deployment binary is about 450 KB. Because it is static, the device needs
no particular C library version; it needs a Linux kernel new enough for the
toolchain's C library (any kernel from the last decade) and a CPU matching
the flags in section 4.3.

---

## 2. Requirements at a glance

| Need | For which targets | Debian / Ubuntu package |
|---|---|---|
| A Linux build environment with `bash`, `mkdir`, `grep` and `file` | all | `bash`, `coreutils`, `grep`, `file` (present on any normal install) |
| A native C compiler, `gcc` | `native` | `build-essential` |
| An ARM cross compiler, `arm-linux-gnueabihf-gcc`, with a **static** C library and the Linux headers for ARM | `armhf` | `gcc-arm-linux-gnueabihf` (pulls `binutils-arm-linux-gnueabihf`, `libc6-dev-armhf-cross` and `linux-libc-dev-armhf-cross`) |
| `arm-linux-gnueabihf-readelf` | `armhf` (post-build check only) | `binutils-arm-linux-gnueabihf` |
| `qemu-arm-static` | optional: run the ARM binary on the build machine | `qemu-user-static` |

The build was developed and tested with **GCC 13.3.0** for both the native
and the cross compiler, from the Ubuntu 24.04 packages named above. Any GCC
from 4.9 onward that understands `-std=c99`, `-march=armv7-a` and
`-mfpu=neon-vfpv4` should work; nothing in the source needs a newer compiler.

---

## 3. Getting the tools

### 3.1 A Linux build environment

The build script is a `bash` script and the compilers are GNU toolchains, so
the build runs on Linux. Any of these will do:

- **A Linux machine or virtual machine.** Debian and Ubuntu are the worked
  examples below because their repositories carry a complete ARM cross
  toolchain in one package.
- **Windows Subsystem for Linux (WSL)** on Windows 10 or 11, with any
  distribution that has the packages below. Install instructions:
  <https://learn.microsoft.com/windows/wsl/install>.
- **A container** (Docker, Podman) from a Debian or Ubuntu image, with the
  source directory mounted in.

macOS has no packaged glibc cross toolchain for this target; use a container
or a virtual machine there.

### 3.2 The native compiler (GCC)

- Upstream: <https://gcc.gnu.org/>
- Debian / Ubuntu: `sudo apt-get install build-essential file`
- Fedora: `sudo dnf groupinstall "Development Tools"` and `sudo dnf install file`

`build.sh` calls the compiler as `gcc`. The `file` utility is used by the
post-build check to describe the binary.

### 3.3 The ARM cross toolchain

The `armhf` target needs a GCC that produces **ARM 32-bit, little-endian,
hard-float, glibc** code (the `arm-linux-gnueabihf` triplet) and that ships
the **static** C library (`libc.a`), because the binary is linked with
`-static`. It also needs the Linux kernel's user-space headers for ARM
(`linux/fb.h`, `linux/input.h`); every complete Linux toolchain includes them.

**Option A - distribution packages (recommended on Debian / Ubuntu).** One
package brings the compiler, the binutils, the static and shared C library
and the kernel headers, all under the exact names `build.sh` expects:

```
sudo apt-get install gcc-arm-linux-gnueabihf
```

Package pages: <https://packages.ubuntu.com/gcc-arm-linux-gnueabihf> and
<https://packages.debian.org/gcc-arm-linux-gnueabihf>. Confirm the install
with `arm-linux-gnueabihf-gcc --version`.

**Option B - the Arm GNU Toolchain (Arm's own binary release).** Download the
`arm-none-linux-gnueabihf` variant for your host from
<https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>, unpack
it anywhere, and put its `bin/` directory on `PATH`. Its tools carry the
prefix `arm-none-linux-gnueabihf-`, so create the two names the script uses
(see "Toolchains with a different prefix" below).

**Option C - Bootlin toolchains.** <https://toolchains.bootlin.com/> offers
prebuilt `armv7-eabihf` glibc toolchains. Pick the glibc variant (the "stable"
release), unpack it, put its `bin/` on `PATH`, and add the two names the
script uses if the toolchain's own prefix differs.

**Option D - musl instead of glibc (smaller binary, not the tested
configuration).** <https://musl.cc/> publishes `arm-linux-musleabihf-cross`.
musl links statically by nature and its binaries are noticeably smaller. The
authors build and test with glibc; a musl build is expected to work but has
not been verified.

**Toolchains with a different prefix.** `build.sh` invokes the compiler and
the ELF reader by the fixed names `arm-linux-gnueabihf-gcc` and
`arm-linux-gnueabihf-readelf`. For a toolchain whose tools are named
differently, put two symbolic links with those names somewhere on `PATH`,
pointing at the toolchain's own binaries. For example, for the Arm GNU
Toolchain unpacked under `~/toolchains/arm-gnu`:

```
mkdir -p ~/bin
ln -s ~/toolchains/arm-gnu/bin/arm-none-linux-gnueabihf-gcc     ~/bin/arm-linux-gnueabihf-gcc
ln -s ~/toolchains/arm-gnu/bin/arm-none-linux-gnueabihf-readelf ~/bin/arm-linux-gnueabihf-readelf
export PATH="$HOME/bin:$PATH"
```

**Check that the toolchain can link statically** before the first build:

```
echo 'int main(void){return 0;}' > /tmp/t.c
arm-linux-gnueabihf-gcc -static -o /tmp/t /tmp/t.c && file /tmp/t
```

`file` must report `statically linked`. A toolchain without `libc.a` fails
this with `cannot find -lc` or similar; on Debian / Ubuntu the fix is
`sudo apt-get install libc6-dev-armhf-cross`.

### 3.4 Optional: QEMU user-mode emulation

With `qemu-arm-static` installed the ARM binary runs on the build machine,
which is enough to check `--version`, `--help`, the option parsing and the
network side of a connection. The framebuffer and touch devices do not exist
under user-mode emulation, so a full run still needs the device.

- Upstream: <https://www.qemu.org/download/>
- Debian / Ubuntu: `sudo apt-get install qemu-user-static`

`build.sh` reports after an `armhf` build whether QEMU was found. It is never
required to build.

---

## 4. Running the build

### 4.1 Steps

1. Unpack or clone the source. Open a shell in the `PrimeVNC` directory (the
   one containing `build.sh`, `src/` and `test/`).
2. Run the target you want:

   ```
   bash build.sh            # the device binary (same as: bash build.sh armhf)
   bash build.sh native     # host build
   ```

   Running it as `bash build.sh` avoids depending on the executable bit,
   which a copy through Windows or a zip archive may have dropped;
   `chmod +x build.sh && ./build.sh` is equivalent.
3. Outputs land in `build/`, which the script creates. Nothing outside the
   `PrimeVNC` directory is written.

### 4.2 What a successful `armhf` build prints

The script echoes the compiler command, then runs the checks below. The
lines to look for:

```
build.sh: target=armhf  cc=arm-linux-gnueabihf-gcc
build.sh: built .../build/primevnc-armhf
----- file -----
.../build/primevnc-armhf: ELF 32-bit LSB executable, ARM, EABI5 version 1 (GNU/Linux), statically linked, ...
----- readelf -A (CPU arch / float ABI) -----
  Tag_CPU_arch: v7
  Tag_FP_arch: VFPv4
  Tag_ABI_VFP_args: VFP registers
----- readelf -d (dynamic NEEDED - should be empty for a static build) -----
(no NEEDED entries - statically linked)
build.sh: OK
```

Three things matter in that output. `statically linked` and the absence of
`NEEDED` entries mean the binary carries its own C library and depends on
nothing on the device. `Tag_CPU_arch: v7` with `Tag_FP_arch: VFPv4` and
`VFP registers` mean the code is ARMv7-A hard-float, which is what the
device's kernel and CPU expect. A compiler warning is a defect: the tree
builds with zero warnings under `-Wall -Wextra -Wshadow -Wpointer-arith`.

### 4.3 The compiler flags, and adapting them

`build.sh` uses these flags for every target:

```
-std=c99 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wpointer-arith
```

`-D_GNU_SOURCE` is given on the command line, before any header is read, so
that `poll`, `mmap`, `sigaction` and the evdev interface are visible under
strict C99. For the `armhf` target it adds:

```
-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -static
```

These select an ARMv7-A core with NEON and VFPv4, which covers Cortex-A5,
A7, A15 and A17. For a core with NEON but only VFPv3 (Cortex-A8, A9) change
`-mfpu=neon-vfpv4` to `-mfpu=neon`; for a core without NEON use
`-mfpu=vfpv3-d16`. A binary built for a floating-point unit the CPU does not
have dies with `Illegal instruction` at the first such instruction, which
may be well into the program. The flags live in one place in `build.sh`
(the `armhf)` case), so an adaptation is a one-line edit.

For a 64-bit ARM device, build with `aarch64-linux-gnu-gcc` and drop the
three ARM-specific flags, keeping `-static`. The protocol, network, input and
DES code is portable C; the framebuffer code in `src/framebuffer.c` calls
the Allwinner sunxi driver's custom ioctls described in
`SUNXI_FB_INTERFACE.md`, and that is the file to review for a different
display driver.

### 4.4 Building without the script

For integration into another build system, the whole program is one
compiler invocation. The source list is the one `build.sh` declares:

```
arm-linux-gnueabihf-gcc -std=c99 -D_GNU_SOURCE -O2 \
    -Wall -Wextra -Wshadow -Wpointer-arith \
    -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -static \
    -o primevnc \
    src/main.c src/log.c src/net.c src/des.c src/pixfmt.c \
    src/framebuffer.c src/input.c src/rfb.c
```

There are no generated files, no configure step and no dependencies to
resolve. Keep the source list explicit in whatever build system you use.

### 4.5 Exit codes of `build.sh`

| Code | Meaning |
|---|---|
| 0 | Built |
| 2 | Unknown target name |
| 3 | The compiler for the chosen target was not found on `PATH` |
| other | The compiler's own exit code; its messages are above the `BUILD FAILED` line |

---

## 5. Running the client off the device

**The native build** (`bash build.sh native`) is the full client for the
build machine. It is dynamically linked and is for developing the protocol
core; to run it you need a Linux framebuffer device and an evdev pointer
device on that machine, which a desktop session normally does not offer. It
connects to any VNC server that serves RAW 32 bpp true colour with VNC
authentication or no authentication, so a desktop VNC server on the same
machine or LAN is a sufficient partner for protocol work; the display path
itself is proven only on the device.

---

## 6. Deploying to a device

The `armhf` binary is one file with no dependencies. Copy it to the device
(for example with `scp` or `sftp`), make it executable, and run it as a user
that can open the framebuffer, the touch device and, for the backlight
controls, the display control device - on most embedded Linux systems that
is `root`:

```
scp build/primevnc-armhf root@<device>:/tmp/primevnc
ssh root@<device> 'chmod +x /tmp/primevnc && /tmp/primevnc --version'
```

The command line, the exit codes and what a launcher around the client must
do are in `README.md`. Whatever program owns the display must be stopped
before the client starts, and restarted after it exits; the client itself
does neither. `SUNXI_FB_INTERFACE.md` documents the framebuffer driver
behaviour the client depends on.

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `build.sh: compiler 'arm-linux-gnueabihf-gcc' not found in PATH` (exit 3) | No cross toolchain, or one under another name | Install per section 3.3, or add the two symbolic links described there |
| `cannot find -lc`, `cannot find crt1.o`, or `libc.a: No such file` when linking `armhf` | The toolchain has no static C library | Debian / Ubuntu: `sudo apt-get install libc6-dev-armhf-cross`; other toolchains: choose a release that ships `libc.a` |
| `linux/fb.h: No such file or directory` or `linux/input.h: No such file or directory` | The toolchain lacks the Linux user-space headers | Debian / Ubuntu: `sudo apt-get install linux-libc-dev-armhf-cross`; other toolchains: use a Linux (not bare-metal) variant |
| `/usr/bin/env: 'bash\r': No such file or directory` or `$'\r': command not found` | `build.sh` was converted to CRLF line endings on the way in | `dos2unix build.sh` (package `dos2unix`), or run it as `bash build.sh` after converting |
| `bash: ./build.sh: Permission denied` | The executable bit was lost in a copy | Run `bash build.sh`, or `chmod +x build.sh` |
| `arm-linux-gnueabihf-readelf: command not found` after a successful compile | Cross binutils missing | `sudo apt-get install binutils-arm-linux-gnueabihf`. The binary is already built; only the post-build check was skipped |
| The binary prints `Illegal instruction` on the device | The CPU lacks NEON or VFPv4 | Adjust `-mfpu` as in section 4.3 and rebuild |
| The binary reports `No such file or directory` on the device although it is plainly there | It was built dynamically (a `native` build copied by mistake, or `-static` removed) and the device lacks the loader it names | Copy `build/primevnc-armhf` from an `armhf` build; check with `file` that it says `statically linked` |
| Warnings during the build | A compiler newer than the tested one, or a local change | The tree is warning-free under the flags in section 4.3; treat a warning as a defect to fix, not noise |

---

## 8. Summary

On Debian or Ubuntu (natively, in a container, or under WSL) the whole
setup is:

```
sudo apt-get install build-essential file gcc-arm-linux-gnueabihf
cd PrimeVNC
bash build.sh           # the device binary -> build/primevnc-armhf
```

Everything else in this document is for other toolchains and other targets.
