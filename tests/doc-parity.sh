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
	shift 2
	"$ORACLE" -convert doc "$@" -output "$tmp/o.doc" "$src" >/dev/null 2>&1 || {
		echo "SKIP $name (oracle failed)"; return; }
	"$MY" "$@" "$src" "$tmp/m.doc" || { echo "FAIL $name (docgen failed)"; fail=$((fail+1)); return; }
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

# Run-wide styling: -font / -fontsize over plain text.
run_case "style-font-arial"    "$tmp/simple.txt" -font Arial
run_case "style-font-times"    "$tmp/simple.txt" -font "Times New Roman"
run_case "style-font-courier"  "$tmp/simple.txt" -font Courier
run_case "style-font-geneva"   "$tmp/simple.txt" -font Geneva
run_case "style-font-zapfino"  "$tmp/simple.txt" -font Zapfino
run_case "style-font-symbol"   "$tmp/simple.txt" -font Symbol
run_case "style-font-dingbats" "$tmp/simple.txt" -font "Zapf Dingbats"
run_case "style-font-menlo"    "$tmp/simple.txt" -font Menlo
run_case "style-size-30"       "$tmp/simple.txt" -fontsize 30
run_case "style-size-9"        "$tmp/simple.txt" -fontsize 9
run_case "style-size-10_5"     "$tmp/simple.txt" -fontsize 10.5
run_case "style-arial-30"      "$tmp/simple.txt" -font Arial -fontsize 30
run_case "style-geneva-18"     "$tmp/simple.txt" -font Geneva -fontsize 18
run_case "style-2paras"        "$tmp/two_paras.txt" -font Courier -fontsize 24
run_case "style-unicode"       "$tmp/unicode.txt" -font Arial -fontsize 14
run_case "style-empty-f"       "$tmp/empty.txt" -font Arial
run_case "style-empty-s"       "$tmp/empty.txt" -fontsize 30

# Character runs that mix fonts and sizes (what textutil produces from RTF or
# HTML).  Fixtures are written with python3 so the run lists stay readable.
python3 - "$tmp" <<'PY'
import sys, os
d = sys.argv[1]

def w(name, s):
    with open(os.path.join(d, name), "w") as f:
        f.write(s)

# Two fonts, two sizes.
w("runs_a.txt", "Hello World\n")
w("runs_a.runs", "0|6|Times New Roman|12\n6|5|Arial|24\n")
w("runs_a.rtf", r"{\rtf1\ansi\deff0{\fonttbl{\f0\froman Times New Roman;}"
  r"{\f1\fswiss Arial;}}\f0\fs24 Hello \f1\fs48 World\par}")

# Forty one-character runs -> several character FKP pages.
xs = "".join("\\fs%d X" % s for s in range(24, 64, 2)) * 2
w("runs_f.txt", "X" * 40 + "\n")
w("runs_f.runs", "".join("%d|1|Times New Roman|%d\n" % (i, 12 + i % 20)
  for i in range(40)))
w("runs_f.rtf", r"{\rtf1\ansi\deff0{\fonttbl{\f0\froman Times New Roman;}}"
  r"\f0 " + xs + r"\par}")

# Stress: 600 characters, 150 runs, five fonts, several text/FKP pages.
fonts = ["Times New Roman", "Arial", "Courier New", "Georgia", "Verdana"]
sizes = [12, 14, 18, 24, 36]
ft = "".join("{\\f%d\\fnil %s;}" % (i, f) for i, f in enumerate(fonts))
txt = []
rtf = []
runs = []
pos = ri = 0
while pos < 600:
    L = min(4, 600 - pos)
    f = fonts[ri % len(fonts)]
    s = sizes[ri % len(sizes)]
    seg = "".join(chr(65 + (pos + j) % 26) for j in range(L))
    txt.append(seg)
    rtf.append("\\f%d\\fs%d %s" % (fonts.index(f), s * 2, seg))
    runs.append("%d|%d|%s|%d" % (pos, L, f, s))
    pos += L
    ri += 1
w("runs_s.txt", "".join(txt) + "\n")
w("runs_s.runs", "\n".join(runs) + "\n")
w("runs_s.rtf", r"{\rtf1\ansi\deff0{\fonttbl" + ft + "}" + "".join(rtf)
  + r"\par}")
PY

run_multi() {
	name="$1"
	"$ORACLE" -convert doc "$tmp/$name.rtf" -output "$tmp/o.doc" \
	    >/dev/null 2>&1 || { echo "SKIP $name (oracle failed)"; return; }
	"$MY" -runs "$tmp/$name.runs" "$tmp/$name.txt" "$tmp/m.doc" \
	    || { echo "FAIL $name (docgen failed)"; fail=$((fail+1)); return; }
	if cmp -s "$tmp/o.doc" "$tmp/m.doc"; then
		pass=$((pass+1))
	else
		echo "FAIL $name"
		fail=$((fail+1))
	fi
}

run_multi runs_a
run_multi runs_f
run_multi runs_s

echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
