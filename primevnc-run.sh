#!/bin/sh
# ---------------------------------------------------------------------------
# primevnc-run.sh -- reference on-device launcher for PrimeVNC.
#
# Stops the device's stock framebuffer UI so the client owns the panel, then
# runs PrimeVNC against the VNC server you name. When the client exits, run
# primevnc-restore-ui.sh to bring the stock UI back.
#
# This is a SAMPLE for the reference device (an Elegoo Centauri 2 / ec-eeb001
# class printer, Allwinner sunxi Linux). The PrimeVNC client itself knows
# nothing about any UI; this glue script is what stops and starts it, and it is
# the only device-specific part. Adapt UI (the process name) and the binary
# location for another target.
#
# Usage:
#   primevnc-run.sh <ip> [password] [--background] [extra primevnc options...]
#
#   <ip>          numeric IPv4 of the VNC server (no DNS)
#   [password]    OPTIONAL VNC password. Give it for a server that uses VNC
#                 Authentication; it is handed to the client through the
#                 environment so it does NOT appear in the process list (ps).
#                 OMIT it for a server with no password (RFB "None" security):
#                 then nothing is passed and the client connects unauthenticated.
#                 When present it is the argument right after <ip> that does NOT
#                 begin with a dash -- a leading-dash argument is taken as an
#                 option, so a no-password launch goes straight to the options,
#                 e.g. "primevnc-run.sh <ip> --background".
#   --background  detach and run the client in the background so it keeps
#                 running after you log out of this SSH session (the device has
#                 no nohup or setsid). RUNTIME detach ONLY: it installs nothing,
#                 so it does NOT survive a reboot -- after a reboot the device
#                 comes up running its own stock UI, untouched by this.
#   extra args    anything else is passed straight to primevnc, e.g.
#                 --port 5901, --rotate 90, --backlight-auto 60
#
# The user supplies <ip> (and optionally a password) on this script's command
# line and they are forwarded to the client; nothing is hard-coded here.
#
# This is the exact procedure used to launch and test the client on the device:
#   1. kill the stock UI (it ignores SIGTERM, so -9) to free /dev/fb0 and the
#      touch input node;
#   2. run the client -- in the FOREGROUND by default, so this script IS the
#      session; or, with --background, detached so it outlives an SSH logout.
#      Either way, when the client returns (a clean stop, or the reconnect
#      budget exhausted) the panel is left idle until you run
#      primevnc-restore-ui.sh.
#
# In the FOREGROUND the client's exit code is returned unchanged:
#   0 clean stop or reconnect budget exhausted   1 usage error
#   2 no framebuffer                              3 no touch device
# With --background the script returns 0 once the client is launched (a usage
# error still exits 1 first); the detached client's own exit code is not
# available to the caller.
# ---------------------------------------------------------------------------

# The stock UI's process name on the reference device.
UI=ec-eeb001-gui

# Require the server address; the password is optional (a no-password server
# uses RFB "None" security and needs none).
if [ "$#" -lt 1 ]; then
    echo "usage: $0 <ip> [password] [--background] [primevnc options...]" >&2
    exit 1
fi
SERVER="$1"
shift

# Optional password: the next argument, but ONLY when it is not an option. An
# argument beginning with a dash is an option (e.g. --background), so a launch
# with no password falls straight through to the option scan below.
HAVE_PASSWORD=0
PASSWORD=""
if [ "$#" -ge 1 ]; then
    case "$1" in
        -*)
            : ;;                        # an option, not a password -- leave it
        *)
            PASSWORD="$1"               # a bare word right after <ip> is the password
            HAVE_PASSWORD=1
            shift ;;
    esac
fi

# Pull our own --background flag out of the remaining arguments; everything left
# is forwarded to primevnc untouched (the client has no --background of its own).
# Rotate the whole list once -- shift each argument off the front and re-append
# the keepers to the back -- so word boundaries in the forwarded options are
# preserved and their order is kept.
BACKGROUND=0
argc=$#
i=0
while [ "$i" -lt "$argc" ]; do
    arg="$1"
    shift
    if [ "$arg" = "--background" ]; then
        BACKGROUND=1
    else
        set -- "$@" "$arg"
    fi
    i=$((i + 1))
done

# Find the client: prefer a copy beside this script, otherwise rely on PATH.
DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
if [ -x "$DIR/primevnc" ]; then
    BIN="$DIR/primevnc"
else
    BIN=primevnc
fi

# Stop the stock UI. It ignores SIGTERM, so send SIGKILL; ignore the "no such
# process" case. Then let the kernel release the framebuffer before we open it.
killall -9 "$UI" 2>/dev/null
sleep 1

# If a password was given, hand it to the client through an environment
# variable and --password-env -- so it is never in argv and never shows up in
# ps -- by prepending those options to the forwarded set. With no password we
# prepend nothing, and the client uses the server's "None" security type.
if [ "$HAVE_PASSWORD" -eq 1 ]; then
    PRIMEVNC_PASSWORD="$PASSWORD"
    export PRIMEVNC_PASSWORD
    set -- --password-env PRIMEVNC_PASSWORD "$@"
fi

if [ "$BACKGROUND" -eq 1 ]; then
    # Detached background run. The subshell ignores SIGHUP and sends its stdio
    # to /dev/null, then is backgrounded with &, so it keeps running when this
    # SSH session closes and the shell HUPs its jobs (the device has no nohup or
    # setsid to do this the usual way). This is a RUNTIME detach ONLY: nothing is
    # written to any init or boot location, so a reboot kills it and the device
    # boots to its stock UI as usual. exec inside the subshell makes primevnc
    # itself the backgrounded process, so $! is its pid.
    ( trap '' HUP; exec "$BIN" "$SERVER" "$@" >/dev/null 2>&1 ) &
    echo "primevnc detached (pid $!); stop it with: kill $! (or killall primevnc)"
    exit 0
fi

# Foreground run (default): exec replaces this shell with the client, so the
# client's exit code becomes this script's exit code and this script IS the
# session.
exec "$BIN" "$SERVER" "$@"
