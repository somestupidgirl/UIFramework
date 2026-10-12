#!/usr/bin/env bash
# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
#
# Byte-parity harness for UIFoundation's .doc writer (docgen), compared against
# Apple's /usr/bin/textutil.
#
#   MY=build/release/docgen ORACLE=/usr/bin/textutil bash tests/doc-parity.sh
#
# MY is a path to the docgen driver; a relative path is resolved against the
# project root (the parent of this script's directory).

set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"

MY="${MY:-build/release/docgen}"
case "$MY" in
	/*) ;;
	*) MY="$root/$MY" ;;
esac
ORACLE="${ORACLE:-/usr/bin/textutil}"

if [ ! -x "$MY" ]; then
	echo "error: docgen not found at $MY" >&2
	exit 1
fi
if [ ! -x "$ORACLE" ]; then
	echo "warning: oracle $ORACLE is absent, skipping .doc parity" >&2
	exit 0
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/docparity.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

run_case() {
	name="$1"
	src="$2"
	"$ORACLE" -convert doc -output "$tmp/o.doc" "$src" >/dev/null 2>&1 || {
		echo "SKIP $name (oracle failed)"; return; }
	"$MY" "$src" "$tmp/m.doc" || { echo "FAIL $name (docgen failed)"; fail=$((fail+1)); return; }
	if cmp -s "$tmp/o.doc" "$tmp/m.doc"; then
		pass=$((pass+1))
	else
		echo "FAIL $name"
		fail=$((fail+1))
	fi
}

# Fixtures are written with python3 so control characters and non-BMP
# codepoints survive.
python3 - "$tmp" <<'PY'
import sys, os
d = sys.argv[1]
cases = {
    "empty": b"",
    "lf": b"\n",
    "cr": b"\r",
    "crlf": b"\r\n",
    "space": b" ",
    "two_lf": b"\n\n",
    "simple": b"hello\n",
    "x": b"x",
    "x_lf": b"x\n",
    "lf_x": b"\nx\n",
    "two_paras": b"one\ntwo\n",
    "trailing_lf": b"a\n\n",
    "unicode": "h\u00e9llo \u65e5\u672c\u8a9e\n".encode(),
    "emoji": "\U0001f600\U0001f601\n".encode(),
    "bom_only": b"\xef\xbb\xbf",
    "bom_text": b"\xef\xbb\xbfhello\n",
    "u2028": "\u2028".encode(),
    "u2029": "\u2029".encode(),
    "u2028x2": "\u2028\u2028".encode(),
    "u2029x2": "\u2029\u2029".encode(),
    "ls_between": b"a\xe2\x80\xa8b",
    "ps_between": b"a\xe2\x80\xa9b",
    "nel": "\u0085".encode(),
    "mid_bom": b"a\xef\xbb\xbfb",
    "lfcr": b"\n\r",
    "crcr": b"\r\r",
    "ctrl": b"a\x0b\x0cb\x07\n",
    "tab": b"a\tb\n",
}
for n in (510, 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 4095, 4096):
    cases["a%d" % n] = b"a" * n
for name, data in cases.items():
    with open(os.path.join(d, name + ".txt"), "wb") as f:
        f.write(data)
PY

while read -r name; do
	[ -n "$name" ] || continue
	run_case "$name" "$tmp/$name.txt"
done <<EOF
$(python3 -c 'import os,sys;print("\n".join(sorted(f[:-4] for f in os.listdir(sys.argv[1]) if f.endswith(".txt"))))' "$tmp")
EOF

echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
