#!/usr/bin/env bash
#
# Reads the coredump a panic left in flash and decodes it against the build that
# produced it.
#
#   scripts/pull-coredump.sh                 # read and decode
#   scripts/pull-coredump.sh --erase         # ...then erase, so the next one is fresh
#   scripts/pull-coredump.sh --dbg           # interactive gdb on the dump
#
# Use this instead of reading the serial console when the radar panics. The
# console is the least reliable witness there is: panic output goes out through
# panic_print_char, which spins waiting for TX FIFO space, and this board is
# native USB CDC (ARDUINO_USB_MODE=1 in platformio.ini). If the host stops
# draining that endpoint the panic handler blocks there until the 5s task
# watchdog resets the chip mid-character, and the whole thing starts again. That
# is what an endless reboot loop with truncated register dumps actually is -- a
# panic that cannot finish printing, not necessarily dozens of distinct crashes.
# The dump in flash is written before any of that and survives it.
#
# The console is also a poor witness for a second reason: the backtrace it
# prints is unwound across the interrupt vectors, which produces plausible-
# looking frames at ROM addresses that no code ever branched to. The coredump
# carries every task's real stack, so you get the task that actually died, what
# the other five were doing, and how much stack each had left.
#
# Requires the port to itself -- close the serial monitor first, or esptool
# reports "the port is busy or doesn't exist". The radar is left running.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# PlatformIO's own interpreter, for the same reason release.sh uses it: the
# project .venv has an esptool that breaks on this board.
PENV="${PENV:-$HOME/.platformio/penv/bin}"
ESPTOOL="${ESPTOOL:-$PENV/esptool.py}"
ESPCOREDUMP="${ESPCOREDUMP:-$PENV/esp-coredump}"

# esp-coredump shells out to gdb and finds it by name on PATH -- it has no flag
# for this, and without it fails with "GDB executable not found. Please install
# GDB or set up ESP-IDF", which is misleading because PlatformIO ships two.
#
# It has to be this one. Both tool-xtensa-esp-elf-gdb and toolchain-xtensa-
# esp32s3 provide a binary called xtensa-esp32s3-elf-gdb, and both run, but the
# toolchain's is gdb 9.2 from 2020 and esp-coredump rejects it with that same
# "GDB executable not found" message rather than a version complaint. The one
# here is gdb 17.1. If this ever breaks, check the version before anything else:
#   $GDB_BIN/xtensa-esp32s3-elf-gdb --version
GDB_BIN="${GDB_BIN:-$HOME/.platformio/packages/tool-xtensa-esp-elf-gdb/bin}"

PIOENV="${PIOENV:-esp32-s3-gc9b72}"
ELF="$ROOT/.pio/build/$PIOENV/firmware.elf"
PARTITIONS="$ROOT/partitions_ota.csv"
OUTDIR="$ROOT/build/coredump"

ERASE=0
DBG=0
PORT=""

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --erase) ERASE=1 ;;
    --dbg)   DBG=1 ;;
    -p|--port) shift; PORT="${1:-}" ;;
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

[ -x "$ESPTOOL" ]     || die "esptool not found at $ESPTOOL"
[ -x "$ESPCOREDUMP" ] || die "esp-coredump not found at $ESPCOREDUMP -- pip install esp-coredump into PlatformIO's penv"
[ -d "$GDB_BIN" ]     || die "xtensa toolchain not found at $GDB_BIN"

# Decoding against the wrong build produces confident, wrong symbols rather than
# an error, so refuse to guess. The panic prints "ELF file SHA256: <16 hex>";
# this is the same value, and comparing them is the only way to know the symbols
# below belong to the firmware that crashed.
[ -f "$ELF" ] || die "no build at $ELF -- build the env that is on the device, and do not rebuild after the crash"
ELF_SHA="$(shasum -a 256 "$ELF" | cut -c1-16)"

# The offset is read from the partition table rather than hardcoded because it
# has already moved once: under huge_app.csv coredump sat at 0x3F0000, and
# partitions_ota.csv relocated it to 0x7F0000. A stale constant here would
# silently read the wrong region and report "no coredump" forever.
[ -f "$PARTITIONS" ] || die "no partition table at $PARTITIONS"
read -r CD_OFF CD_SIZE <<EOF
$(awk -F',' '
  /^[[:space:]]*#/ { next }
  {
    gsub(/[[:space:]]/, "", $1); gsub(/[[:space:]]/, "", $2)
    gsub(/[[:space:]]/, "", $4); gsub(/[[:space:]]/, "", $5)
    if ($1 == "coredump" && $2 == "data") { print $4, $5; exit }
  }' "$PARTITIONS")
EOF
[ -n "${CD_OFF:-}" ] || die "no coredump partition in $PARTITIONS"

if [ -z "$PORT" ]; then
  # A plain loop rather than mapfile: macOS ships bash 3.2, which has neither
  # mapfile nor readarray, and this script is run on the machine with the board
  # plugged into it.
  PORTS=""
  NPORTS=0
  for p in /dev/cu.usbmodem*; do
    [ -e "$p" ] || continue
    PORTS="$PORTS  $p
"
    NPORTS=$((NPORTS + 1))
    PORT="$p"
  done
  [ "$NPORTS" -gt 0 ] || die "no /dev/cu.usbmodem* port found -- is the radar plugged in?"
  if [ "$NPORTS" -gt 1 ]; then
    echo "Several boards are connected. Pick one with -p:" >&2
    printf '%s' "$PORTS" >&2
    exit 1
  fi
fi

mkdir -p "$OUTDIR"
RAW="$OUTDIR/coredump.bin"
CORE_ELF="$OUTDIR/core.elf"

echo "Port          : $PORT"
echo "Partition     : $CD_OFF ($CD_SIZE)"
echo "Firmware ELF  : $ELF"
echo "ELF SHA256    : $ELF_SHA   <-- must match the panic's \"ELF file SHA256:\" line"
echo

# --after no_reset keeps the chip in the bootloader between this read and the
# erase below, so a crash-looping board cannot panic again underneath us.
"$ESPTOOL" --port "$PORT" --connect-attempts 3 --after no_reset \
  read_flash "$CD_OFF" "$CD_SIZE" "$RAW" >/dev/null 2>&1 \
  || { "$ESPTOOL" --port "$PORT" --after hard_reset chip_id >/dev/null 2>&1 || true
       die "could not read flash -- close the serial monitor if it is holding $PORT"; }

# An erased partition is the normal state on a board that has never panicked.
# Say so plainly instead of handing esp-coredump 64KB of 0xFF and letting it
# fail with a checksum error that reads like something is broken.
if ! python3 - "$RAW" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
sys.exit(0 if set(d) - {0xFF} else 1)
PY
then
  echo "No coredump stored -- the partition is erased."
  echo "That means no panic has written one since the last erase, not that the crash was clean."
  "$ESPTOOL" --port "$PORT" --after hard_reset chip_id >/dev/null 2>&1 || true
  exit 0
fi

echo "Decoding $(wc -c < "$RAW" | tr -d ' ') bytes..."
echo

export PATH="$GDB_BIN:$PATH"
SUBCMD=info_corefile
[ "$DBG" -eq 1 ] && SUBCMD=dbg_corefile

set +e
"$ESPCOREDUMP" --chip esp32s3 "$SUBCMD" \
  --core "$RAW" --core-format raw --save-core "$CORE_ELF" "$ELF"
STATUS=$?
set -e

if [ "$ERASE" -eq 1 ] && [ "$STATUS" -eq 0 ]; then
  # Nothing in the dump records when it was written, and the panic handler only
  # writes one if the partition can hold it -- so the surest way to know the
  # next dump you read belongs to the next crash is to clear this one now.
  echo
  echo "Erasing $CD_OFF so the next panic writes a fresh dump..."
  "$ESPTOOL" --port "$PORT" --connect-attempts 3 --after no_reset \
    erase_region "$CD_OFF" "$CD_SIZE" >/dev/null 2>&1 \
    || echo "warning: erase failed -- the next dump you read may be this one again" >&2
fi

# Always hand the board back running, including after a decode failure. Leaving
# it parked in the bootloader looks exactly like a dead radar.
"$ESPTOOL" --port "$PORT" --after hard_reset chip_id >/dev/null 2>&1 || true

echo
echo "Raw dump  : $RAW"
[ -f "$CORE_ELF" ] && echo "Core ELF  : $CORE_ELF"
echo "Radar restarted."
exit "$STATUS"
