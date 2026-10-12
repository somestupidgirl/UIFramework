# UIFoundation

Open-source, clean-room reimplementation of Apple's `UIFoundation` framework,
part of the LibreDarwin effort. The goal is byte-for-byte compatible output
against the shipping framework.

## What is here

The first component is the Word 97-2003 (`.doc`) writer that
`NSDocFormatWriter` drives through
`-[NSAttributedString dataFromRange:documentAttributes:]`, for the single-run
document that `textutil -convert doc` produces: plain text, optionally with one
run-wide font and size (the `-font` and `-fontsize` options). It is exposed as
a small C API, `UIDocFormatWriterGenerate()` / `UIDocFormatWriterGenerateStyled()`,
so plain-C tools such as `textutil` can link it without an Objective-C runtime:

- `include/UIFoundation/NSDocFormatWriter.h` - the public API.
- `src/NSDocFormatWriter.c` - UTF-8 text to a Compound File Binary (CFB/OLE2)
  container holding `WordDocument`, `1Table`, `SummaryInformation` and
  `DocumentSummaryInformation`.
- `src/*.h` - the constant stream templates, extracted from the shipping
  framework's own template blobs.

The writer's structure was recovered from the disassembly of
`-[NSDocFormatWriter docFormatData]` and its write helpers: the four streams are
assembled from static templates, the FIB and 1Table are patched with the
character count, the font table (SttbfFfn) and the run's character properties
are written for the selected font and size, and the CFB
sector/FAT/directory layout is reproduced directly, including its minimum of
eight 512-byte sectors per stream.

## Building

```
make                   # release build into build/release/
make CONFIG=debug      # debug build into build/debug/
make doc-parity        # byte-compare the .doc writer against /usr/bin/textutil
make clean
```

The `Makefile` is portable to both GNU make and BSD make (bmake): no pattern
rules, no `ifeq`/`ifdef`/`.if` conditionals and no `$(if)`/`$(shell)`
functions; per-configuration flags come from `make/<CONFIG>.mk`. An equivalent
Xcode project lives in `UIFoundation.xcodeproj`.

The build produces `build/<CONFIG>/UIFoundation.framework` and a small
`docgen` test driver that links it. `textutil` (in the sibling AKCmds
checkout) links the same framework for its `.doc` output.

## Testing

`tests/doc-parity.sh` generates a battery of plain-text fixtures (plus run-wide
`-font` / `-fontsize` cases), runs both `/usr/bin/textutil -convert doc` and
`docgen` over each, and compares every byte. It is wired to `make doc-parity`
and to the `doc-parity` Xcode target.

## License

BSD-3-Clause. See `LICENSE`.
