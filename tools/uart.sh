#!/usr/bin/env bash
# Plain serial monitor. Prints whatever the board says, nothing else.
#
# Usage: ./uart.sh [port] [baud]
#        ./uart.sh                  -> first /dev/ttyUSB*, 115200
#        ./uart.sh /dev/ttyUSB0 74880
#
# Keys:  Ctrl-C quit      Ctrl-T timestamps on/off      Ctrl-R reset pulse
#
# DTR and RTS are held deasserted. On adapters that wire them to EN/IO0 the
# defaults would hold the chip in reset, which looks exactly like a dead board.

set -uo pipefail

PORT="${1:-}"
BAUD="${2:-115200}"

if [ -z "$PORT" ]; then
    for p in /dev/ttyUSB* /dev/ttyACM*; do [ -e "$p" ] && { PORT="$p"; break; }; done
fi
[ -n "$PORT" ] || { echo "No serial port found. Plug the adapter in, or pass one explicitly."; exit 1; }

exec python3 - "$PORT" "$BAUD" <<'PY'
import serial, sys, time, termios, tty, select, os

port, baud = sys.argv[1], int(sys.argv[2])
try:
    s = serial.Serial(port, baud, timeout=0.05)
except Exception as e:
    print(f"cannot open {port}: {e}"); sys.exit(1)

s.setDTR(False); s.setRTS(False)

print(f"--- {port} @ {baud} --- Ctrl-C quit, Ctrl-T timestamps, Ctrl-R reset ---",
      flush=True)

stamps = False
at_line_start = True
fd = sys.stdin.fileno()
interactive = os.isatty(fd)
old = termios.tcgetattr(fd) if interactive else None
if interactive:
    tty.setcbreak(fd)

def out(text):
    global at_line_start
    if not stamps:
        sys.stdout.write(text)
    else:
        # Prefix every line, not every chunk: the UART arrives in ragged pieces.
        for ch in text:
            if at_line_start and ch not in '\r\n':
                sys.stdout.write(time.strftime('[%H:%M:%S] '))
                at_line_start = False
            sys.stdout.write(ch)
            if ch == '\n':
                at_line_start = True
    sys.stdout.flush()

try:
    while True:
        d = s.read(4096)
        if d:
            out(d.decode('utf-8', 'replace'))

        if interactive and select.select([fd], [], [], 0)[0]:
            k = os.read(fd, 1)
            if k == b'\x03':                       # Ctrl-C
                break
            elif k == b'\x14':                     # Ctrl-T
                stamps = not stamps
                print(f"\n--- timestamps {'on' if stamps else 'off'} ---", flush=True)
            elif k == b'\x12':                     # Ctrl-R
                # Only does anything if RTS is actually wired to EN.
                s.setRTS(True); time.sleep(0.12); s.setRTS(False)
                print("\n--- reset pulse sent on RTS ---", flush=True)
            else:
                s.write(k)                         # forward keystrokes to the board
except KeyboardInterrupt:
    pass
finally:
    if interactive:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
    s.close()
    print("\n--- closed ---")
PY
