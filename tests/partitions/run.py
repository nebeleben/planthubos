#!/usr/bin/env python3
"""Validate every shipped partition table against its target flash size.

Uses ESP-IDF's own gen_esp32part.py -- the same check the firmware build
runs at link time, but in under a second and with no toolchain. A wrong
table is a bricking-class defect (a first install writes the app to a
hardcoded offset), so it gets a test that does not depend on anyone
remembering to run a full build for every target.

Deliberately pure Python rather than shell+awk: macOS ships BSD awk, which
has no strtonum(), and this repo has already been bitten by assuming GNU
userland.

Run: python3 planthubos/tests/partitions/run.py
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.normpath(os.path.join(HERE, "..", ".."))
IDF = os.environ.get("IDF_PATH") or os.path.expanduser("~/esp/esp-idf-v5.5")
GEN = os.path.join(IDF, "components", "partition_table", "gen_esp32part.py")

# table filename -> (flash size passed to the generator, exact end offset)
TABLES = {
    "partitions_16m.csv": ("16MB", 0x1000000),
    "partitions_c5.csv": ("4MB", 0x400000),
}


def size_to_bytes(text):
    m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)([KMkm]?)", text.strip())
    if not m:
        raise ValueError("unparseable size %r" % text)
    return int(m.group(1), 0) * {"": 1, "K": 1024, "M": 1024 * 1024}[m.group(2).upper()]


def check(name, flash_size, expected_end):
    csv = os.path.join(FW, name)
    if not os.path.exists(csv):
        return "FAIL %s: no such file" % name
    out = os.path.join(HERE, name + ".bin")

    gen = subprocess.run([sys.executable, GEN, "--flash-size", flash_size, csv, out],
                         capture_output=True, text=True)
    if gen.returncode != 0:
        detail = (gen.stderr.strip() or gen.stdout.strip()).splitlines()
        return "FAIL %s (%s): generator rejected: %s" % (
            name, flash_size, detail[-1] if detail else "no output")

    dump = subprocess.run([sys.executable, GEN, out], capture_output=True, text=True)
    if dump.returncode != 0:
        if os.path.exists(out):
            os.remove(out)
        return "FAIL %s: generated table could not be read back" % name

    # Parse partitions in order, checking for gaps and final boundary
    partitions = []
    end = 0
    for line in dump.stdout.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        fields = line.split(",")
        if len(fields) < 5:
            continue
        part_name = fields[0].strip()
        part_offset = int(fields[3], 0)
        part_size = size_to_bytes(fields[4])
        part_end = part_offset + part_size
        partitions.append((part_name, part_offset, part_size, part_end))
        end = max(end, part_end)

    if os.path.exists(out):
        os.remove(out)

    # Check for gaps between consecutive partitions (allow up to 64K for alignment)
    for i in range(1, len(partitions)):
        prev_name, prev_offset, prev_size, prev_end = partitions[i-1]
        curr_name, curr_offset, curr_size, curr_end = partitions[i]
        gap = curr_offset - prev_end
        if gap > 0x10000:  # Larger than 64K alignment boundary
            return "FAIL %s: gap between %s and %s — %s ends at %#x, %s starts at %#x" % (
                name, prev_name, curr_name, prev_name, prev_end, curr_name, curr_offset)

    if end != expected_end:
        return "FAIL %s: table ends at %#x, expected exactly %#x (%s)" % (
            name, end, expected_end, "leaves flash unused" if end < expected_end else "overruns")
    return "PASS %s (%s): valid, ends exactly at %#x" % (name, flash_size, end)


def main():
    if not os.path.exists(GEN):
        print("FAIL: gen_esp32part.py not found at %s (set IDF_PATH)" % GEN)
        return 1
    failed = 0
    for name, (flash_size, expected_end) in sorted(TABLES.items()):
        result = check(name, flash_size, expected_end)
        print(result)
        if result.startswith("FAIL"):
            failed = 1
    return failed


if __name__ == "__main__":
    sys.exit(main())
