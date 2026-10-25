#!/bin/sh
# ---------------------------------------------------------------------------
# primevnc-restore-ui.sh -- bring the device's stock UI back after PrimeVNC.
#
# Companion to primevnc-run.sh. Run this once the VNC session has ended to
# restart the stock framebuffer UI (ec-eeb001-gui on the reference device).
#
# Why this is not simply "ec-eeb001-gui &": the device's init starts the UI with
# a specific environment -- its PATH and, critically, its LD_LIBRARY_PATH -- and
# without that a relaunch from an ssh shell fails SILENTLY. The bare name is not
# on the shell's PATH, and the full path cannot load its libraries (it dies on
# libfreetype.so.6) without LD_LIBRARY_PATH. That was found the hard way while
# testing on the device. So this script reconstructs that environment -- it
# prefers the exact environment of a still-running Elegoo daemon (which init
# launched correctly) and falls back to the reference device's known locations --
# then launches the UI in a HUP-ignoring subshell with its stdio on /dev/null, so
# the UI survives the SSH session that ran this script closing. (The box has
# neither setsid nor nohup, so that subshell is how we detach -- the same form
# primevnc-run.sh's --background uses.)
#
# Usage: primevnc-restore-ui.sh
#
# SAMPLE for the reference device; adapt the UI name, its binary path and the
# library path for another target.
# ---------------------------------------------------------------------------

# The stock UI's process name and full path on the reference device.
UI=ec-eeb001-gui
UI_BIN=/opt/bin/ec-eeb001-gui

# Already up? Nothing to do.
if pidof "$UI" >/dev/null 2>&1; then
    echo "$UI is already running" >&2
    exit 0
fi

# Prefer the environment init actually gave the UI, by copying the three
# variables that matter (PATH, LD_LIBRARY_PATH, HOME) from a running Elegoo
# daemon -- the printer host, which init started with the correct paths.
REF=$(pidof elegoo_printer 2>/dev/null | awk '{print $1}')
if [ -n "$REF" ] && [ -r "/proc/$REF/environ" ]; then
    for kv in $(tr '\0' '\n' < "/proc/$REF/environ" | grep -E '^(PATH|LD_LIBRARY_PATH|HOME)='); do
        export "$kv"
    done
fi

# Fill anything still unset with the reference device's known locations. The UI's
# own libraries live under /opt/lib, which is what must be on LD_LIBRARY_PATH.
[ -n "$LD_LIBRARY_PATH" ] || LD_LIBRARY_PATH=/opt/lib:/lib:/usr/lib
[ -n "$PATH" ] || PATH=/opt/bin:/usr/sbin:/usr/bin:/sbin:/bin
[ -n "$HOME" ] || HOME=/root
export LD_LIBRARY_PATH PATH HOME

# Launch the UI detached. The subshell ignores SIGHUP and puts stdin and both
# outputs on /dev/null, then execs the UI so the UI itself becomes that process
# (so $! is its pid). The ignored HUP is inherited across the exec, which is what
# lets the UI survive the SSH session that ran this script closing -- without it a
# relaunch from an ssh command shell can be HUP'd when that session ends. The box
# has neither setsid nor nohup, so this subshell is the way to detach; it is the
# same form primevnc-run.sh's --background uses.
( trap '' HUP; exec "$UI_BIN" >/dev/null 2>&1 </dev/null ) &

echo "restarted $UI (pid $!)" >&2
