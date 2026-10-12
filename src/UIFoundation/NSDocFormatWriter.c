/*
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Clean-room reimplementation of UIFoundation's NSDocFormatWriter for the
 * single-run document case that textutil -convert doc exercises: plain text,
 * optionally with one run-wide font and size (-font / -fontsize).
 *
 * The layout below was recovered from the disassembly of
 * -[NSDocFormatWriter docFormatData] and its write helpers: the four streams
 * are built from static templates, the FIB and 1Table are patched with the
 * character count, the font table (SttbfFfn) is rebuilt for the run's font,
 * and the CFB container is assembled directly (the writer's own
 * sector/FAT/directory scheme, including its minimum of eight 512-byte
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

/* 1Table geometry: the font table (SttbfFfn) starts here and is immediately
 * followed by the doc-info block, which is a constant that the writer simply
 * relocates when the font table's length changes. */
#define FONT_TABLE_OFF  0xEF
#define DOCINFO_OFF     0x1ED
#define DOCINFO_LEN     0x202

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
 * The body font table (SttbfFfn).  It is always seeded with the three fonts
 * Apple's writer emits by default; the run's own font is appended only when it
 * is not already one of them.  A font's charset byte is 2 for the symbol
 * fonts and 0 otherwise.
 */
static const char *const kSeedFonts[3] = {
	"Times New Roman", "Symbol", "Arial"
};

static const char *const kSymbolFonts[] = {
	"Symbol", "Zapf Dingbats", "Webdings", "Wingdings"
};

static int font_is_symbol(const char *name)
{
	for (size_t i = 0; i < sizeof(kSymbolFonts) / sizeof(kSymbolFonts[0]); i++)
		if (strcmp(name, kSymbolFonts[i]) == 0)
			return 1;
	return 0;
}

/* UTF-16 code unit count of a UTF-8 string */
static size_t utf16_len(const char *s)
{
	uint32_t *cp = NULL;
	size_t n = 0, units = 0;
	if (utf8_decode((const unsigned char *)s, strlen(s), &cp, &n) != 0)
		return 0;
	for (size_t i = 0; i < n; i++)
		units += cp[i] >= 0x10000 ? 2 : 1;
	free(cp);
	return units;
}

/*
 * Write a SttbfFfn entry for name at p and return the number of bytes written.
 * The entry is a 0x28-byte fixed header (length byte, flag 6, weight 400,
 * charset, then zero padding) followed by the UTF-16LE name and a terminator.
 */
static size_t put_ffn(unsigned char *p, const char *name)
{
	uint32_t *cp = NULL;
	size_t n = 0, units = 0;
	if (utf8_decode((const unsigned char *)name, strlen(name), &cp, &n) != 0)
		return 0;
	for (size_t i = 0; i < n; i++)
		units += cp[i] >= 0x10000 ? 2 : 1;

	memset(p, 0, 0x28);
	p[0] = (unsigned char)((2 * units + 0x29) & 0xff);
	p[1] = 6;
	put_u16(p + 2, 400);
	p[4] = (unsigned char)(font_is_symbol(name) ? 2 : 0);

	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = cp[i];
		if (c >= 0x10000) {
			c -= 0x10000;
			put_u16(p + 0x28 + 2 * k, (uint16_t)(0xd800 + (c >> 10)));
			put_u16(p + 0x28 + 2 * k + 2, (uint16_t)(0xdc00 + (c & 0x3ff)));
			k += 2;
		} else {
			put_u16(p + 0x28 + 2 * k, (uint16_t)c);
			k++;
		}
	}
	put_u16(p + 0x28 + 2 * k, 0);
	free(cp);
	return 0x28 + 2 * k + 2;
}

/*
 * Build the font table for a run whose font is runfont (NULL means the default
 * body font).  Stores the table length, the index of the run's font and the
 * font list count.  Returns NULL on allocation failure.
 */
static unsigned char *build_font_table(const char *runfont, size_t *outLen,
    int *outIndex, int *outCount)
{
	const char *names[4];
	if (!runfont)
		runfont = "Helvetica Light";

	int count = 3;
	for (int i = 0; i < 3; i++)
		names[i] = kSeedFonts[i];
	int index = -1;
	for (int i = 0; i < 3; i++)
		if (strcmp(runfont, names[i]) == 0)
			index = i;
	if (index < 0) {
		names[3] = runfont;
		index = 3;
		count = 4;
	}

	size_t len = 4;
	for (int i = 0; i < count; i++)
		len += 0x2a + 2 * utf16_len(names[i]);

	unsigned char *t = calloc(1, len);
	if (!t)
		return NULL;
	put_u32(t, (uint32_t)count);
	size_t off = 4;
	for (int i = 0; i < count; i++)
		off += put_ffn(t + off, names[i]);

	*outLen = len;
	*outIndex = index;
	*outCount = count;
	return t;
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

/*
 * Build the WordDocument stream.  fontTableLen and docInfoOff are the two
 * 1Table offsets the FIB records (the length of the font table and the start
 * of the doc-info block); fontIndex and hps select the run's font and
 * half-point size in the character FKP's CHPX.
 */
static unsigned char *build_word(const uint16_t *units, size_t ccp,
    int fontIndex, uint16_t hps, size_t fontTableLen, size_t docInfoOff,
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
	put_u16(wd + 0x116, (unsigned)fontTableLen);
	put_u16(wd + 0x192, (unsigned)docInfoOff);
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

	/*
	 * The character FKP's single CHPX encodes the run font as two 2-byte
	 * sprm operands and the size as the sprmCHps operand (half-points).
	 */
	unsigned char *cfkp = wd + b0;
	put_u16(cfkp + 0x1F5, (unsigned)fontIndex);
	put_u16(cfkp + 0x1F9, (unsigned)fontIndex);
	put_u16(cfkp + 0x1FD, hps);

	*outLen = cbmac;
	return wd;
}

/*
 * Build the 1Table stream.  For an empty document the constant empty template
 * is used; otherwise the character-count patches are applied and the font
 * table (plus the relocated doc-info block) is written after them.
 */
static unsigned char *build_table(size_t ccp, int isEmpty,
    const unsigned char *fontTable, size_t fontTableLen, size_t *outLen)
{
	unsigned char *t = malloc(TABLE_LEN);
	if (!t)
		return NULL;

	if (isEmpty) {
		memcpy(t, table1_empty_4096, TABLE_LEN);
		*outLen = TABLE_LEN;
		return t;
	}

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

	/*
	 * Overwrite the font table and rewrite the doc-info block that follows
	 * it, then clear whatever the shorter table left behind.
	 */
	size_t docOff = FONT_TABLE_OFF + fontTableLen;
	memcpy(t + FONT_TABLE_OFF, fontTable, fontTableLen);
	memcpy(t + docOff, table1_nonempty_4096 + DOCINFO_OFF, DOCINFO_LEN);
	size_t end = docOff + DOCINFO_LEN;
	if (end < TABLE_LEN)
		memset(t + end, 0, TABLE_LEN - end);

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

int UIDocFormatWriterGenerateStyled(const char *utf8, size_t len,
    const UIDocFormatWriterStyle *style, unsigned char **out, size_t *outLen)
{
	int isEmpty = 0;
	size_t ccp = 0;
	uint16_t *units = prepare_text(utf8, len, &ccp, &isEmpty);
	if (!units)
		return -1;

	/*
	 * The empty document ignores any requested styling and uses its own
	 * constant template, whose font table is 12 bytes shorter than the
	 * standard one (its body font is "Helvetica", not "Helvetica Light").
	 */
	const char *runfont = style ? style->fontName : NULL;
	double pt = (style && style->pointSize > 0) ? style->pointSize : 12.0;
	uint16_t hps = isEmpty ? 24 : (uint16_t)(pt * 2.0 + 0.01);

	int fontIndex = 3;
	size_t fontTableLen = 0xf2, docInfoOff = 0x1e1;   /* empty-template values */
	unsigned char *fontTable = NULL;
	if (!isEmpty) {
		int count = 0;
		fontTable = build_font_table(runfont, &fontTableLen, &fontIndex,
		    &count);
		if (!fontTable) { free(units); return -1; }
		docInfoOff = FONT_TABLE_OFF + fontTableLen;
	}

	size_t wdLen = 0, t1Len = 0, siLen = 0, dsiLen = 0;
	unsigned char *wd = build_word(units, ccp, fontIndex, hps, fontTableLen,
	    docInfoOff, &wdLen);
	unsigned char *t1 = build_table(ccp, isEmpty, fontTable, fontTableLen,
	    &t1Len);
	unsigned char *si = malloc(TABLE_LEN);
	unsigned char *dsi = malloc(TABLE_LEN);
	free(units);
	free(fontTable);

	if (!wd || !t1 || !si || !dsi) {
		free(wd); free(t1); free(si); free(dsi);
		return -1;
	}
	memcpy(si, si_4096, TABLE_LEN);
	memcpy(dsi, dsi_4096, TABLE_LEN);
	siLen = dsiLen = TABLE_LEN;

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

int UIDocFormatWriterGenerate(const char *utf8, size_t len,
    unsigned char **out, size_t *outLen)
{
	return UIDocFormatWriterGenerateStyled(utf8, len, NULL, out, outLen);
}
