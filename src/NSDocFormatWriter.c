/*
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Clean-room reimplementation of UIFoundation's NSDocFormatWriter for the
 * plain, unstyled document case that textutil -convert doc exercises.
 *
 * The layout below was recovered from the disassembly of
 * -[NSDocFormatWriter docFormatData] and its write helpers: the four streams
 * are built from static templates, the FIB and 1Table are patched with the
 * character count, and the CFB container is assembled directly (the writer's
 * own sector/FAT/directory scheme, including its minimum of eight 512-byte
 * sectors per stream).
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "UIFoundation/NSDocFormatWriter.h"

#include "doc_head_0600.h"
#include "doc_char_fkp_0200.h"
#include "doc_para_fkp_0200.h"
#include "doc_desc_0200.h"
#include "table1_nonempty_4096.h"
#include "table1_empty_4096.h"
#include "si_4096.h"
#include "dsi_4096.h"
#include "doc_hdr_0050.h"
#include "doc_dir_0280.h"

#define HEAD_LEN     0x600
#define FKP_LEN      0x200
#define TABLE_LEN    4096
#define HDR_LEN      0x50
#define DIR_LEN      0x280
#define SECTOR       512

static void put_u16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void put_u32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
	p[2] = (unsigned char)((v >> 16) & 0xff);
	p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_u64(unsigned char *p, uint64_t v)
{
	put_u32(p, (uint32_t)(v & 0xffffffffu));
	put_u32(p + 4, (uint32_t)(v >> 32));
}

static size_t align_up(size_t x, size_t n)
{
	return (x + n - 1) / n * n;
}

static size_t sectors_for(size_t len)
{
	size_t s = (len + SECTOR - 1) / SECTOR;
	return s < 8 ? 8 : s;
}

/* ---- UTF-8 -> codepoints ------------------------------------------------ */

static int utf8_decode(const unsigned char *s, size_t n, uint32_t **out,
    size_t *outCount)
{
	uint32_t *cp = malloc((n + 1) * sizeof(uint32_t));
	if (!cp)
		return -1;
	size_t i = 0, k = 0;
	while (i < n) {
		unsigned char c = s[i];
		uint32_t v;
		size_t need;
		if (c < 0x80) { v = c; need = 0; }
		else if ((c & 0xe0) == 0xc0) { v = c & 0x1f; need = 1; }
		else if ((c & 0xf0) == 0xe0) { v = c & 0x0f; need = 2; }
		else if ((c & 0xf8) == 0xf0) { v = c & 0x07; need = 3; }
		else { v = 0xfffd; need = 0; }
		i++;
		for (size_t j = 0; j < need; j++) {
			if (i >= n || (s[i] & 0xc0) != 0x80) { v = 0xfffd; break; }
			v = (v << 6) | (s[i] & 0x3f);
			i++;
		}
		cp[k++] = v;
	}
	*out = cp;
	*outCount = k;
	return 0;
}

static size_t utf16_units(const uint32_t *cp, size_t n, uint16_t **out)
{
	uint16_t *u = malloc((n * 2 + 2) * sizeof(uint16_t));
	if (!u)
		return 0;
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = cp[i];
		if (c >= 0x10000) {
			c -= 0x10000;
			u[k++] = (uint16_t)(0xd800 + (c >> 10));
			u[k++] = (uint16_t)(0xdc00 + (c & 0x3ff));
		} else {
			u[k++] = (uint16_t)c;
		}
	}
	*out = u;
	return k;
}

/*
 * Turn raw UTF-8 into the text Apple's writer stores: paragraph breaks become
 * CR (with a single trailing one dropped), soft line separators become 0x0B,
 * and an all-empty result becomes " \r".  *isEmpty reports whether the empty
 * template applies (i.e. the character count before normalisation is zero).
 */
static uint16_t *prepare_text(const char *utf8, size_t len, size_t *outCount,
    int *isEmpty)
{
	uint32_t *cp = NULL;
	size_t n = 0;
	if (utf8_decode((const unsigned char *)utf8, len, &cp, &n) != 0)
		return NULL;

	/* strip a single leading BOM */
	if (n > 0 && cp[0] == 0xfeff)
		memmove(cp, cp + 1, (n - 1) * sizeof(uint32_t)), n--;

	/* the empty template applies only when the string has no characters */
	*isEmpty = (n == 0);

	/*
	 * Map the input to Word's character model: CR, LF and U+2029 are
	 * paragraph breaks (they become CR), while U+2028 and U+0085 are soft
	 * line separators (they become vertical tab, 0x0B).
	 */
	uint32_t *norm = malloc((n + 1) * sizeof(uint32_t));
	if (!norm) { free(cp); return NULL; }
	size_t m = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = cp[i];
		if (c == 0x0d || c == 0x0a || c == 0x2029)
			norm[m++] = 0x0d;
		else if (c == 0x2028 || c == 0x0085)
			norm[m++] = 0x0b;
		else
			norm[m++] = c;
	}
	free(cp);

	/* drop a single trailing paragraph break */
	if (m > 0 && norm[m - 1] == 0x0d)
		m--;

	if (m == 0) {
		free(norm);
		uint16_t *u = malloc(2 * sizeof(uint16_t));
		if (!u) return NULL;
		u[0] = 0x20; u[1] = 0x0d;
		*outCount = 2;
		return u;
	}

	/* a terminating paragraph mark */
	norm[m++] = 0x0d;

	uint16_t *u = NULL;
	size_t units = utf16_units(norm, m, &u);
	free(norm);
	if (!u) return NULL;
	*outCount = units;
	*isEmpty = 0;
	return u;
}

/* ---- stream builders ---------------------------------------------------- */

static unsigned char *build_word(const uint16_t *units, size_t ccp,
    size_t *outLen)
{
	size_t text_end = 0x600 + 2 * ccp;
	size_t cbmac = align_up(text_end, 0x200) + 0x600;
	unsigned char *wd = calloc(1, cbmac);
	if (!wd)
		return NULL;

	memcpy(wd, doc_head_0600, HEAD_LEN);
	put_u16(wd + 0x1c, (unsigned)(0x600 + 2 * ccp));
	put_u32(wd + 0x40, (uint32_t)cbmac);
	put_u32(wd + 0x4c, (uint32_t)ccp);
	for (size_t i = 0; i < ccp; i++)
		put_u16(wd + 0x600 + 2 * i, units[i]);

	size_t b0 = align_up(text_end, 0x200);
	const unsigned char *blocks[3] = {
		doc_char_fkp_0200, doc_para_fkp_0200, doc_desc_0200
	};
	for (int i = 0; i < 3; i++) {
		unsigned char *dst = wd + b0 + (size_t)i * 0x200;
		memcpy(dst, blocks[i], FKP_LEN);
		put_u32(dst, 0x600);
		put_u32(dst + 4, (uint32_t)text_end);
	}
	*outLen = cbmac;
	return wd;
}

static unsigned char *build_table(size_t ccp, int isEmpty, size_t *outLen)
{
	unsigned char *t = malloc(TABLE_LEN);
	if (!t)
		return NULL;
	if (isEmpty) {
		memcpy(t, table1_empty_4096, TABLE_LEN);
	} else {
		memcpy(t, table1_nonempty_4096, TABLE_LEN);
		unsigned k = (unsigned)((2 * ccp - 1) >> 9);
		put_u32(t + 0xA6, (uint32_t)ccp);
		put_u32(t + 0xBA, (uint32_t)(0x600 + 2 * ccp));
		t[0xBE] = (unsigned char)(4 + k);
		put_u32(t + 0xC6, (uint32_t)(0x600 + 2 * ccp));
		t[0xCA] = (unsigned char)(5 + k);
		put_u32(t + 0xD2, (uint32_t)(0x600 + 2 * ccp));
		t[0xD6] = (unsigned char)(6 + k);
		put_u32(t + 0xE3, (uint32_t)ccp);
	}
	*outLen = TABLE_LEN;
	return t;
}

/* ---- CFB assembly ------------------------------------------------------- */

static unsigned char *build_cfb(const unsigned char *wd, size_t wdLen,
    const unsigned char *t1, size_t t1Len, const unsigned char *si,
    size_t siLen, const unsigned char *dsi, size_t dsiLen, size_t *outLen)
{
	size_t wsec = sectors_for(wdLen);
	size_t tsec = sectors_for(t1Len);
	size_t ssec = sectors_for(siLen);
	size_t dsec = sectors_for(dsiLen);
	size_t ssum = wsec + tsec + ssec + dsec;
	size_t fatsec = (ssum + 127) / 128;
	size_t total = ssum + fatsec + 5;
	size_t file = total * SECTOR;

	unsigned char *out = calloc(1, file);
	if (!out)
		return NULL;

	/* header */
	unsigned char *hdr = out;
	memcpy(hdr, doc_hdr_0050, HDR_LEN);
	memset(hdr + HDR_LEN, 0xff, SECTOR - HDR_LEN);
	put_u32(hdr + 0x2c, (uint32_t)fatsec);
	put_u32(hdr + 0x30, (uint32_t)(ssum + fatsec));
	put_u32(hdr + 0x3c, (uint32_t)(ssum + fatsec + 2));
	put_u32(hdr + 0x40, 1);
	for (size_t i = 0; i < fatsec; i++)
		put_u32(hdr + 0x4c + 4 * i, (uint32_t)(ssum + i));

	/* streams (sector s lives at (s+1)*512) */
	memcpy(out + (0 + 1) * SECTOR, wd, wdLen);
	memcpy(out + (wsec + 1) * SECTOR, t1, t1Len);
	memcpy(out + (wsec + tsec + 1) * SECTOR, si, siLen);
	memcpy(out + (wsec + tsec + ssec + 1) * SECTOR, dsi, dsiLen);

	/* FAT */
	size_t fatEntries = fatsec * 128;
	uint32_t *fat = malloc(fatEntries * sizeof(uint32_t));
	if (!fat) { free(out); return NULL; }
	for (size_t i = 0; i < fatEntries; i++)
		fat[i] = 0xffffffffu;
	size_t chains[4][2] = {
		{ 0, wsec }, { wsec, tsec }, { wsec + tsec, ssec },
		{ wsec + tsec + ssec, dsec }
	};
	for (int c = 0; c < 4; c++) {
		size_t start = chains[c][0], count = chains[c][1];
		for (size_t i = 0; i < count; i++)
			fat[start + i] = (i < count - 1) ?
			    (uint32_t)(start + i + 1) : 0xfffffffeu;
	}
	for (size_t i = 0; i < fatsec; i++)
		fat[ssum + i] = (i < fatsec - 1) ?
		    (uint32_t)(ssum + i + 1) : 0xfffffffeu;
	fat[ssum + fatsec] = (uint32_t)(ssum + fatsec + 1);
	fat[ssum + fatsec + 1] = 0xfffffffeu;
	fat[ssum + fatsec + 2] = 0xfffffffeu;
	fat[ssum + fatsec + 3] = 0xfffffffeu;

	unsigned char *fatBytes = out + (ssum + 1) * SECTOR;
	for (size_t i = 0; i < fatEntries; i++)
		put_u32(fatBytes + 4 * i, fat[i]);
	free(fat);

	/* directory */
	unsigned char *dir = out + (ssum + fatsec + 1) * SECTOR;
	memcpy(dir, doc_dir_0280, DIR_LEN);
	struct { size_t start, size; } ent[5] = {
		{ ssum + fatsec + 3, 0x80 },
		{ wsec, tsec * SECTOR },
		{ 0, wsec * SECTOR },
		{ wsec + tsec, ssec * SECTOR },
		{ wsec + tsec + ssec, dsec * SECTOR },
	};
	for (int i = 0; i < 5; i++) {
		put_u32(dir + i * 0x80 + 0x74, (uint32_t)ent[i].start);
		put_u64(dir + i * 0x80 + 0x78, (uint64_t)ent[i].size);
	}
	/* miniFAT and mini stream stay zero */

	*outLen = file;
	return out;
}

/* ---- public entry point ------------------------------------------------- */

int UIDocFormatWriterGenerate(const char *utf8, size_t len,
    unsigned char **out, size_t *outLen)
{
	int isEmpty = 0;
	size_t ccp = 0;
	uint16_t *units = prepare_text(utf8, len, &ccp, &isEmpty);
	if (!units)
		return -1;

	size_t wdLen = 0, t1Len = 0, siLen = 0, dsiLen = 0;
	unsigned char *wd = build_word(units, ccp, &wdLen);
	unsigned char *t1 = build_table(ccp, isEmpty, &t1Len);
	unsigned char *si = malloc(TABLE_LEN);
	unsigned char *dsi = malloc(TABLE_LEN);
	free(units);

	if (!wd || !t1 || !si || !dsi) {
		free(wd); free(t1); free(si); free(dsi);
		return -1;
	}
	memcpy(si, si_4096, TABLE_LEN);
	memcpy(dsi, dsi_4096, TABLE_LEN);
	siLen = dsiLen = TABLE_LEN;
	/* the empty template also trims 12 bytes off two 1Table offsets */
	if (isEmpty) {
		wd[0x116] -= 12;
		wd[0x192] -= 12;
	}

	size_t file = 0;
	unsigned char *cfb = build_cfb(wd, wdLen, t1, t1Len, si, siLen, dsi,
	    dsiLen, &file);
	free(wd); free(t1); free(si); free(dsi);
	if (!cfb)
		return -1;

	*out = cfb;
	*outLen = file;
	return 0;
}
