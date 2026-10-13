/*
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Driver for the .doc writer parity harness.  Usage:
 *
 *   docgen [-font NAME] [-fontsize PT] [-runs FILE] INPUT OUTPUT
 *
 * Reads UTF-8 text from INPUT and writes a .doc image to OUTPUT.  A run-wide
 * font and size may be applied with -font / -fontsize (matching textutil's
 * options for unattributed input); for body text that mixes fonts and sizes,
 * -runs names a file of character runs, one per line:
 *
 *   start|length|font|size
 *
 * where start / length are UTF-16 code-unit offsets into the normalised body
 * (see NSDocFormatWriter.h), font is "-" for the default body font and size is
 * empty or 0 for the default size.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "UIFoundation/NSDocFormatWriter.h"

static char *read_file(const char *path, size_t *outLen)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *b;
	if (!f) {
		perror(path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (!b) { fclose(f); return NULL; }
	if (fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	b[n] = 0;
	*outLen = (size_t)n;
	return b;
}

/*
 * Parse the run file into an array of runs.  Returns the array (caller frees
 * it and the duplicated font names) or NULL on failure.  *outCount receives the
 * number of runs.
 */
static UIDocFormatWriterRun *read_runs(const char *path, size_t *outCount)
{
	size_t n = 0;
	char *text = read_file(path, &n);
	UIDocFormatWriterRun *runs = NULL;
	size_t count = 0, cap = 0;
	char *line, *save, *fsave;

	if (!text)
		return NULL;
	for (line = strtok_r(text, "\n", &save); line;
	    line = strtok_r(NULL, "\n", &save)) {
		char *f[4];
		int nf = 0;
		char *tok;
		for (tok = strtok_r(line, "|", &fsave)
		    ; tok && nf < 4
		    ; tok = strtok_r(NULL, "|", &fsave))
			f[nf++] = tok;
		if (nf < 2)
			continue;
		if (count == cap) {
			cap = cap ? cap * 2 : 8;
			runs = realloc(runs, cap * sizeof(*runs));
			if (!runs) { free(text); return NULL; }
		}
		runs[count].characterIndex = (size_t)strtoul(f[0], NULL, 10);
		runs[count].characterCount = (size_t)strtoul(f[1], NULL, 10);
		runs[count].fontName = (nf >= 3 && strcmp(f[2], "-") != 0)
		    ? strdup(f[2]) : NULL;
		runs[count].pointSize = (nf >= 4) ? atof(f[3]) : 0.0;
		count++;
	}
	free(text);
	*outCount = count;
	return runs;
}

int main(int argc, char **argv)
{
	const char *font = NULL;
	const char *runsFile = NULL;
	double size = 0.0;
	const char *positional[2] = { NULL, NULL };
	int npos = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-font") == 0 && i + 1 < argc) {
			font = argv[++i];
		} else if (strcmp(argv[i], "-fontsize") == 0 && i + 1 < argc) {
			size = atof(argv[++i]);
		} else if (strcmp(argv[i], "-runs") == 0 && i + 1 < argc) {
			runsFile = argv[++i];
		} else if (npos < 2) {
			positional[npos++] = argv[i];
		} else {
			fprintf(stderr, "docgen: unexpected argument %s\n",
			    argv[i]);
			return 2;
		}
	}
	if (npos != 2) {
		fprintf(stderr, "docgen: usage: docgen [-font NAME] "
		    "[-fontsize PT] [-runs FILE] INPUT OUTPUT\n");
		return 2;
	}

	size_t n = 0;
	char *b = read_file(positional[0], &n);
	if (!b)
		return 1;

	unsigned char *out = NULL;
	size_t outLen = 0;
	int rc;

	if (runsFile) {
		size_t runCount = 0;
		UIDocFormatWriterRun *runs = read_runs(runsFile, &runCount);
		if (!runs) { free(b); return 1; }
		rc = UIDocFormatWriterGenerateRuns(b, n, runs, runCount, &out,
		    &outLen);
		for (size_t i = 0; i < runCount; i++)
			free((void *)runs[i].fontName);
		free(runs);
	} else {
		UIDocFormatWriterStyle style = { font, size };
		rc = UIDocFormatWriterGenerateStyled(b, n, &style, &out, &outLen);
	}
	if (rc != 0) {
		fprintf(stderr, "gen fail\n");
		free(b);
		return 1;
	}

	FILE *g = fopen(positional[1], "wb");
	if (!g) { perror(positional[1]); free(b); free(out); return 1; }
	fwrite(out, 1, outLen, g);
	fclose(g);
	free(b);
	free(out);
	return 0;
}
