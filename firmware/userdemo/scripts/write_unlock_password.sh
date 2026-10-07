#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/cu.usbmodem21101}"
IDF_PATH="${IDF_PATH:-/Users/fanshuqing/esp/esp-idf-v5.5.4}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVS_OFFSET="0x9000"
NVS_SIZE="0x4000"

echo "Project: $PROJECT_ROOT"
echo "Port:    $PORT"
echo "IDF:     $IDF_PATH"
echo
echo "This will flash the app, then rewrite the NVS partition at $NVS_OFFSET."
echo "StopWatch must be in download mode. Press Ctrl-C to cancel."
read -r -p "Continue? [y/N] " yn
case "$yn" in
  [yY]|[yY][eE][sS]) ;;
  *) echo "Cancelled."; exit 0 ;;
esac

printf "Unlock password: "
IFS= read -r -s PASSWORD
echo
if [[ -z "$PASSWORD" ]]; then
  echo "Password is empty; aborting." >&2
  exit 1
fi

tmp="$(mktemp -d)"
cleanup() {
  unset PASSWORD
  rm -rf "$tmp"
}
trap cleanup EXIT

csv="$tmp/unlock_nvs.csv"
bin="$tmp/unlock_nvs.bin"

python3 -c '
import csv
import sys

path = sys.argv[1]
pwd = sys.stdin.read()
if pwd.endswith("\n"):
    pwd = pwd[:-1]

with open(path, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["key", "type", "encoding", "value"])
    w.writerow(["unlock", "namespace", "", ""])
    w.writerow(["pwd", "data", "string", pwd])
' "$csv" <<<"$PASSWORD"

cd "$PROJECT_ROOT"
# shellcheck source=/dev/null
. "$IDF_PATH/export.sh"

python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
  generate "$csv" "$bin" "$NVS_SIZE"

idf.py -p "$PORT" flash
python -m esptool --chip esp32s3 -p "$PORT" write_flash "$NVS_OFFSET" "$bin"
python -m esptool --chip esp32s3 -p "$PORT" run

echo
echo "Done. Reboot StopWatch if it remains in download mode. UI should show: Password : stored"
