/*
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Clean-room reimplementation of the Word 97-2003 (.doc) writer that
 * UIFoundation's NSDocFormatWriter drives via
 * -[NSAttributedString dataFromRange:documentAttributes:].
 *
 * The public entry point turns UTF-8 text into a Compound File Binary
 * (CFB / OLE2) container holding the four streams Apple's writer emits for a
 * plain, unstyled document: WordDocument, 1Table, SummaryInformation and
 * DocumentSummaryInformation.
 */

#ifndef UIFOUNDATION_NSDOCFORMATWRITER_H
#define UIFOUNDATION_NSDOCFORMATWRITER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run-wide character styling for the whole body.  A NULL fontName selects the
 * writer's default body font ("Helvetica Light"); a pointSize of 0 selects the
 * default size (12pt).  The style applies to every character in the document,
 * which is what textutil's -font / -fontsize options produce for unattributed
 * input.
 */
typedef struct {
	const char *fontName;   /* UTF-8, or NULL for the default */
	double pointSize;       /* points, or 0 for the default (12pt) */
} UIDocFormatWriterStyle;

/*
 * One character-formatting run, used for styled documents whose body mixes
 * fonts and/or sizes (what textutil produces from RTF or HTML input).
 *
 * characterIndex is a UTF-16 code-unit offset into the writer's normalised
 * body text: the input with a single leading BOM removed and paragraph / line
 * separators mapped (CR, LF and U+2029 become CR; U+2028 and U+0085 become
 * 0x0B), excluding the single trailing paragraph mark that is dropped and the
 * terminating mark that is appended.  Runs must be contiguous and start at 0;
 * the final run extends through the terminating paragraph mark.  A NULL
 * fontName selects the default body font and a pointSize of 0 the default size.
 */
typedef struct {
	size_t characterIndex;
	size_t characterCount;
	const char *fontName;   /* UTF-8, or NULL for the default */
	double pointSize;       /* points, or 0 for the default (12pt) */
} UIDocFormatWriterRun;

/*
 * Generate a .doc image from len bytes of UTF-8 text (the same text a caller
 * would hand to textutil).  On success returns 0 and stores a malloc()'d
 * buffer of *outLen bytes in *out; the caller owns it via free().  On failure
 * returns -1 and leaves *out untouched.
 */
int UIDocFormatWriterGenerate(const char *utf8, size_t len,
    unsigned char **out, size_t *outLen);

/*
 * As UIDocFormatWriterGenerate(), but applies a run-wide style.  A NULL style
 * is equivalent to UIDocFormatWriterGenerate().
 */
int UIDocFormatWriterGenerateStyled(const char *utf8, size_t len,
    const UIDocFormatWriterStyle *style, unsigned char **out, size_t *outLen);

/*
 * As UIDocFormatWriterGenerate(), but applies a list of character runs so the
 * body can mix fonts and sizes.  A runCount of 0 (or a NULL runs pointer) is
 * equivalent to UIDocFormatWriterGenerate().
 */
int UIDocFormatWriterGenerateRuns(const char *utf8, size_t len,
    const UIDocFormatWriterRun *runs, size_t runCount,
    unsigned char **out, size_t *outLen);

#ifdef __cplusplus
}
#endif

#endif /* UIFOUNDATION_NSDOCFORMATWRITER_H */
