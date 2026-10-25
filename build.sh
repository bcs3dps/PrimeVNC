#!/usr/bin/env bash
# =============================================================================
#   build.sh - cross/native build for PrimeVNC (framebuffer VNC client)
#
#   Runs in any Linux environment that has the toolchains (BUILDING.md says
#   which and where to get them).  One configurable script builds every
#   target:
#
#       bash build.sh            # default: static armhf ARMv7 for the sunxi target
#       bash build.sh armhf      # same as default
#       bash build.sh native     # native build for RFB-core work on this host
#
#   The source list is EXPLICIT: every .c file is named here, so removing one
#   fails the build and adding one is deliberate.
#
#   Static linking (armhf) is safe because the client does NO name resolution
#   (it is handed a numeric IP), so glibc's NSS static-link hazard never
#   applies.  Verification (file / readelf) runs after a successful build.
# =============================================================================

set -u

# ----------------------------------------------------------------------------
#   Resolve locations relative to this script.
# ----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
OUT_DIR="$SCRIPT_DIR/build"

# ----------------------------------------------------------------------------
#   Explicit source list - one line per translation unit, no wildcards.
# ----------------------------------------------------------------------------
SOURCES="
main.c
log.c
net.c
des.c
pixfmt.c
framebuffer.c
input.c
rfb.c
"

# ----------------------------------------------------------------------------
#   Target selection.
# ----------------------------------------------------------------------------
TARGET="${1:-armhf}"

# ----------------------------------------------------------------------------
#   Common warning / language flags for every target.  _GNU_SOURCE is defined
#   on the command line (before any header is read) so POSIX/Linux APIs -
#   sigaction, poll, mmap, evdev - are visible under the strict -std=c99 mode.
# ----------------------------------------------------------------------------
CFLAGS_COMMON="-std=c99 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wpointer-arith"

mkdir -p "$OUT_DIR"

# ----------------------------------------------------------------------------
#   Assemble the compiler command line for the chosen target.
# ----------------------------------------------------------------------------
case "$TARGET" in
    armhf)
        CC="arm-linux-gnueabihf-gcc"
        READELF="arm-linux-gnueabihf-readelf"
        # ARMv7-A hard-float NEON, statically linked into one self-contained ELF.
        CFLAGS="$CFLAGS_COMMON -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -static"
        OUT_BIN="$OUT_DIR/primevnc-armhf"
        ;;
    native)
        CC="gcc"
        READELF="readelf"
        # Native host build; dynamic is fine here - this target is for RFB-core
        # testing against a PC VNC server, not for deployment.
        CFLAGS="$CFLAGS_COMMON"
        OUT_BIN="$OUT_DIR/primevnc-native"
        ;;
    *)
        echo "build.sh: unknown target '$TARGET' (use 'armhf' or 'native')" >&2
        exit 2
        ;;
esac

# ----------------------------------------------------------------------------
#   Verify the compiler exists before trying to use it.
# ----------------------------------------------------------------------------
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "build.sh: compiler '$CC' not found in PATH" >&2
    exit 3
fi

# ----------------------------------------------------------------------------
#   Build the absolute source path list from the explicit names.
# ----------------------------------------------------------------------------
SRC_PATHS=""
for f in $SOURCES; do
    SRC_PATHS="$SRC_PATHS $SRC_DIR/$f"
done

echo "build.sh: target=$TARGET  cc=$CC"
echo "build.sh: $CC $CFLAGS -o $OUT_BIN <sources>"

# ----------------------------------------------------------------------------
#   Compile and link in one pass.
# ----------------------------------------------------------------------------
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$OUT_BIN" $SRC_PATHS
RC=$?

if [ "$RC" -ne 0 ]; then
    echo "build.sh: BUILD FAILED (rc=$RC)" >&2
    exit "$RC"
fi

echo "build.sh: built $OUT_BIN"

# ----------------------------------------------------------------------------
#   Post-build verification (does not need the target device).
# ----------------------------------------------------------------------------
echo "----- file -----"
file "$OUT_BIN"

if [ "$TARGET" = "armhf" ]; then
    echo "----- readelf -A (CPU arch / float ABI) -----"
    "$READELF" -A "$OUT_BIN" 2>/dev/null | grep -E "Tag_CPU_arch|Tag_ABI_VFP_args|Tag_FP_arch" || true

    echo "----- readelf -d (dynamic NEEDED - should be empty for a static build) -----"
    if "$READELF" -d "$OUT_BIN" 2>/dev/null | grep -q "NEEDED"; then
        "$READELF" -d "$OUT_BIN" 2>/dev/null | grep "NEEDED"
    else
        echo "(no NEEDED entries - statically linked)"
    fi
fi

# ----------------------------------------------------------------------------
#   Smoke-test hint (qemu-user is a one-time optional install, not required
#   to build).  With it the armhf binary's RFB handshake can be run here.
# ----------------------------------------------------------------------------
if [ "$TARGET" = "armhf" ]; then
    if command -v qemu-arm-static >/dev/null 2>&1 || command -v qemu-arm >/dev/null 2>&1; then
        echo "build.sh: qemu-user present - the armhf binary can be smoke-tested on this host."
    else
        echo "build.sh: qemu-user not installed - to smoke-test the armhf binary on this host,"
        echo "          install it once with: sudo apt-get install qemu-user-static"
    fi
fi

echo "build.sh: OK"
exit 0
