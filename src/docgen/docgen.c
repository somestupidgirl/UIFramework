/*
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Driver for the .doc writer parity harness.  Usage:
 *
 *   docgen [-font NAME] [-fontsize PT] INPUT OUTPUT
 *
 * Reads UTF-8 text from INPUT and writes a .doc image to OUTPUT, optionally
 * applying a run-wide font and size (matching textutil's -font / -fontsize).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "UIFoundation/NSDocFormatWriter.h"

int main(int argc, char **argv)
{
	const char *font = NULL;
	double size = 0.0;
	const char *positional[2] = { NULL, NULL };
	int npos = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-font") == 0 && i + 1 < argc) {
			font = argv[++i];
		} else if (strcmp(argv[i], "-fontsize") == 0 && i + 1 < argc) {
			size = atof(argv[++i]);
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
		    "[-fontsize PT] INPUT OUTPUT\n");
		return 2;
	}

	FILE *f = fopen(positional[0], "rb");
	if (!f) { perror(positional[0]); return 1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc(n + 1);
	if (!b) { fclose(f); return 1; }
	if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return 1; }
	fclose(f);
	b[n] = 0;

	UIDocFormatWriterStyle style = { font, size };
	unsigned char *out = NULL;
	size_t outLen = 0;
	if (UIDocFormatWriterGenerateStyled(b, (size_t)n, &style, &out, &outLen)
	    != 0) {
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
