#!/usr/bin/env bash
# Rebuild everything under orig/ and data/ from your own dump.
#
#   bash tools/prepare.sh BASE.nsp UPDATE.nsp prod.keys
#
# Needs the toolchain from
# tools/setup_toolchain.sh (Python packages + llvm-mingw for demangling and
# disassembly listings). The Ghidra project is built separately
# (tools\ghidra\import.cmd), because it takes several minutes per module.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -ne 3 ]; then
  echo "usage: bash tools/prepare.sh BASE.nsp UPDATE.nsp prod.keys" >&2
  exit 2
fi
BASE=$1
UPDATE=$2
KEYS=$3
PY="python -P"
LLVM=toolchain/llvm-mingw/bin

echo "== 1/6 extract ExeFS + patched RomFS (verifies the IVFC hash tree)"
$PY tools/nxextract.py --base "$BASE" --update "$UPDATE" --keys "$KEYS" --out orig

echo "== 2/6 NSO -> ELF"
mkdir -p orig/elf
for m in main sdk subsdk0 subsdk1 rtld; do
  $PY tools/nso2elf.py "orig/exefs/$m" "orig/elf/$m.elf" --json "orig/elf/$m.json" > /dev/null
done

echo "== 3/6 function/import databases + instruction census (main, sdk)"
$PY tools/analyze.py orig/exefs/main --module main --base 0x7100000000
$PY tools/analyze.py orig/exefs/sdk --module sdk --base 0x7180000000 > /dev/null

echo "== 4/6 vtables, call graph, boot reachability"
$PY tools/xrefs.py orig/exefs/main --module main --base 0x7100000000 > /dev/null

echo "== 5/6 string cross-references + process metadata"
$PY tools/strxref.py orig/exefs/main --module main --base 0x7100000000
$PY tools/npdm.py orig/exefs/main.npdm --json data/main_npdm.json > /dev/null

echo "== 6/6 disassembly listings (orig/elf/main.dis, sdk.dis)"
if [ -x "$LLVM/llvm-objdump.exe" ]; then
  "$LLVM/llvm-objdump.exe" -d --no-show-raw-insn orig/elf/main.elf > orig/elf/main.dis
  "$LLVM/llvm-objdump.exe" -d --no-show-raw-insn orig/elf/sdk.elf > orig/elf/sdk.dis
fi
echo "done"
