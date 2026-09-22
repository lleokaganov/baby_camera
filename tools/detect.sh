#!/usr/bin/env bash
# Identify what is attached to the USB-serial ports and, more importantly,
# what state it is in. The three states are easy to confuse from the outside:
#
#   RUNNING     the chip executes its application and talks on the UART
#   BOOTLOADER  the ROM loader waits for a SYNC and stays silent until it gets one
#   DEAD/QUIET  no power, wrong wiring, or the chip is held in reset
#
# esptool alone cannot tell them apart: "No serial data received" is printed
# both when the board is absent and when it is running an application that
# never answers SYNC. So this script listens passively first, then talks.
#
# Usage: ./detect.sh [port]        default: every /dev/ttyUSB* and /dev/ttyACM*

set -uo pipefail

ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
BAUD=115200

ports=()
if [ $# -ge 1 ]; then
    ports=("$1")
else
    for p in /dev/ttyUSB* /dev/ttyACM*; do [ -e "$p" ] && ports+=("$p"); done
fi

if [ ${#ports[@]} -eq 0 ]; then
    echo "No serial ports found."
    echo
    echo "USB devices that look like serial adapters:"
    lsusb | grep -iE '1a86|10c4|0403|303a|2341' || echo "  (none)"
    echo
    echo "If the adapter is plugged in but no port appears, the usual culprit is"
    echo "brltty grabbing CH340 devices:  systemctl status brltty"
    exit 1
fi

for port in "${ports[@]}"; do
    echo "=============================================================="
    echo "PORT  $port"

    # --- which USB device is behind this tty -------------------------------
    dev=$(basename "$port")
    syspath=$(readlink -f "/sys/class/tty/$dev/device" 2>/dev/null)
    usbdir="$syspath"
    vid=""; pid=""; man=""; prod=""
    for _ in 1 2 3 4 5; do
        [ -z "$usbdir" ] && break
        if [ -f "$usbdir/idVendor" ]; then
            vid=$(cat "$usbdir/idVendor"); pid=$(cat "$usbdir/idProduct")
            man=$(cat "$usbdir/manufacturer" 2>/dev/null)
            prod=$(cat "$usbdir/product" 2>/dev/null)
            break
        fi
        usbdir=$(dirname "$usbdir")
    done
    if [ -n "$vid" ]; then
        echo "USB   $vid:$pid  ${man:-?} ${prod:-}"
        case "$vid:$pid" in
            1a86:7523) echo "      CH340 - bare adapter or an ESP32-CAM-MB shield" ;;
            1a86:55d4) echo "      CH9102 - common on newer ESP boards" ;;
            10c4:ea60) echo "      CP2102" ;;
            0403:6001) echo "      FT232" ;;
            303a:*)    echo "      Espressif native USB (S2/S3/C3)" ;;
        esac
    fi

    [ -r "$port" ] && [ -w "$port" ] || { echo "STATE no access to $port (dialout group?)"; continue; }

    # --- listen without touching anything ---------------------------------
    python3 - "$port" "$BAUD" <<'PY'
import serial, sys, time, collections
port, baud = sys.argv[1], int(sys.argv[2])
try:
    s = serial.Serial(port, baud, timeout=0.2)
except Exception as e:
    print("STATE cannot open:", e); sys.exit(0)

# Do not drive DTR/RTS: on boards that wire them to EN/IO0 that would reset
# the chip, and the point here is to observe it untouched.
s.setDTR(False); s.setRTS(False)
time.sleep(0.2); s.reset_input_buffer()

t0 = time.time(); buf = b''
while time.time() - t0 < 3.0:
    d = s.read(256)
    if d: buf += d

if not buf:
    print("QUIET no output in 3 s -- either the ROM loader (silent by design)")
    print("      or no power / wrong wiring")
else:
    hist = collections.Counter(buf)
    top, n = hist.most_common(1)[0]
    printable = sum(1 for b in buf if 9 <= b <= 126)
    print(f"OUTPUT {len(buf)} bytes in 3 s, {len(hist)} distinct values")
    if n > len(buf) * 0.8 and len(hist) <= 2:
        print(f"      almost all 0x{top:02x} ({chr(top) if 32<=top<127 else '?'}) "
              f"at {len(buf)/3:.1f}/s")
        if top == 0x2e:
            print("      => a dot-printing wait loop: the APPLICATION is running.")
            print("         Typical Arduino WiFi wait: while(!connected){delay(400);print('.');}")
    elif printable < len(buf) * 0.5:
        print("      mostly non-printable -- wrong baud rate, or line noise")
        print("      first bytes:", buf[:32].hex())
    else:
        print("      text:", repr(buf[:200]))
    print("STATE RUNNING (an application holds the UART)")
PY

    # --- now try to talk to the ROM loader --------------------------------
    echo "--- esptool, without resetting (works only if already in the loader):"
    timeout 25 python3 "$ESPTOOL" --port "$port" --before no_reset --after no_reset \
        --connect-attempts 2 chip_id 2>&1 | grep -viE '^esptool.py v|^serial port' | sed 's/^/      /'

    echo "--- esptool, with the standard DTR/RTS reset:"
    timeout 25 python3 "$ESPTOOL" --port "$port" --before default_reset --after no_reset \
        --connect-attempts 2 chip_id 2>&1 | grep -viE '^esptool.py v|^serial port' | sed 's/^/      /'
done

cat <<'EOF'
==============================================================
Reading the result

  chip_id succeeded without reset
      The board already sits in the ROM loader. Flash now.

  chip_id succeeded only with reset
      Auto-reset works. Nothing to do by hand, just flash.

  RUNNING plus both attempts failing
      The chip is alive but DTR/RTS do not reach EN/IO0 -- the auto-reset
      wiring on the adapter or shield is absent or broken. Enter the loader
      by hand: hold IO0 low (BOOT button, or a jumper IO0-GND), then reset
      or re-plug USB, then release. IO0 is sampled ONLY at the rising edge
      of reset; pressing BOOT on a board that is already up changes nothing.
      Success looks like the output going completely silent.

  QUIET plus both attempts failing
      Nothing is answering. Check power (5V pin, not 3V3, and enough current
      -- the camera pulls ~300 mA), that the module is seated in the shield,
      and that TX/RX are crossed: adapter TX -> board U0R, adapter RX -> U0T.
EOF
