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

/* The doc-info block is a constant region of the 1Table template that the
 * writer relocates as the font table's length changes. */
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
#define KSEED_COUNT 3
static const char *const kSeedFonts[KSEED_COUNT] = {
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
 * One character-formatting run for the writer's internal plan: where it starts
 * in the body (UTF-16 units), its 0-based index into the font table, and its
 * half-point size.
 */
typedef struct {
	size_t start;
	int fontIndex;
	uint16_t hps;
} body_run;

static int font_lookup(char *const *names, size_t count, const char *name)
{
	for (size_t i = 0; i < count; i++)
		if (strcmp(names[i], name) == 0)
			return (int)i;
	return -1;
}

/*
 * Collect the font list for the run list: the three seed fonts followed by
 * each distinct run font in order of first appearance.  names receives the
 * list (borrowing the caller's strings) and idxPerRun the index of each run's
 * font.  The returned array must be free()d by the caller; NULL on failure.
 */
static char **collect_fonts(const UIDocFormatWriterRun *runs, size_t nruns,
    int *idxPerRun, size_t *outCount)
{
	char **names = malloc((KSEED_COUNT + nruns) * sizeof(char *));
	if (!names)
		return NULL;
	size_t count = 0;
	for (size_t i = 0; i < KSEED_COUNT; i++)
		names[count++] = (char *)kSeedFonts[i];
	for (size_t i = 0; i < nruns; i++) {
		const char *nm = runs[i].fontName ? runs[i].fontName
		    : "Helvetica Light";
		int idx = font_lookup(names, count, nm);
		if (idx < 0) {
			idx = (int)count;
			names[count++] = (char *)nm;
		}
		idxPerRun[i] = idx;
	}
	*outCount = count;
	return names;
}

/*
 * Build the body font table (SttbfFfn) for the given font list and return its
 * length in *outLen (NULL on allocation failure).
 */
static unsigned char *build_font_table(char *const *names, size_t count,
    size_t *outLen)
{
	size_t len = 4;
	for (size_t i = 0; i < count; i++)
		len += 0x2a + 2 * utf16_len(names[i]);

	unsigned char *t = calloc(1, len);
	if (!t)
		return NULL;
	put_u32(t, (uint32_t)count);
	size_t off = 4;
	for (size_t i = 0; i < count; i++)
		off += put_ffn(t + off, names[i]);

	*outLen = len;
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

#define CHPX_LEN 12   /* sprmCRgFtc(0) + sprmCRgFtc(1) + sprmCHps grpprl */

/*
 * Plan how the runs are laid out across the character FKP pages.  Each CHPX
 * block is packed from the top of a 512-byte page; a run starts a new page once
 * the page's CP and offset arrays would meet the blocks (5 bytes per run plus
 * an 8-byte margin).  Fills run_page / run_off (caller arrays of nruns) and
 * allocates *outPageEndCp (npages entries) with the CP at the end of each page.
 */
static int plan_char_fkp(const body_run *runs, size_t nruns, size_t ccp,
    size_t *run_page, size_t *run_off, size_t **outPageEndCp, size_t *outPages)
{
	size_t npage = 0, top = 0x1ff, count = 0;
	for (size_t i = 0; i < nruns; i++) {
		size_t nt = (top - CHPX_LEN - 1) & ~(size_t)1;
		if (count > 0 && !(5 * count + 8 < nt)) {
			npage++;
			top = 0x1ff;
			count = 0;
			nt = (top - CHPX_LEN - 1) & ~(size_t)1;
		}
		run_page[i] = npage;
		run_off[i] = nt;
		top = nt;
		count++;
	}
	size_t pages = npage + 1;
	size_t *endcp = calloc(pages, sizeof(size_t));
	if (!endcp)
		return -1;
	for (size_t i = 0; i < nruns; i++) {
		if (i + 1 < nruns && run_page[i + 1] != run_page[i])
			endcp[run_page[i]] = 0x600 + 2 * runs[i + 1].start;
	}
	endcp[pages - 1] = 0x600 + 2 * ccp;

	*outPageEndCp = endcp;
	*outPages = pages;
	return 0;
}

/*
 * Render the character FKP pages planned above.  Each run's CHPX encodes its
 * font as two sprmCRgFtc operands and its size as the sprmCHps operand
 * (half-points).
 */
static unsigned char *render_char_fkp(const body_run *runs, size_t nruns,
    const size_t *run_page, const size_t *run_off, const size_t *page_end_cp,
    size_t pages)
{
	unsigned char *fk = calloc(pages, FKP_LEN);
	if (!fk)
		return NULL;

	size_t i = 0;
	for (size_t p = 0; p < pages; p++) {
		unsigned char *base = fk + p * FKP_LEN;
		size_t first = i;
		while (i < nruns && run_page[i] == p)
			i++;
		size_t cnt = i - first;

		for (size_t k = 0; k < cnt; k++)
			put_u32(base + 4 * k,
			    (uint32_t)(0x600 + 2 * runs[first + k].start));
		put_u32(base + 4 * cnt, (uint32_t)page_end_cp[p]);

		for (size_t k = 0; k < cnt; k++) {
			size_t idx = first + k;
			unsigned char *b;
			base[4 * (cnt + 1) + k] = (unsigned char)(run_off[idx] >> 1);
			b = base + run_off[idx];
			b[0] = CHPX_LEN;
			b[1] = 0x4f; b[2] = 0x4a;
			put_u16(b + 3, (unsigned)runs[idx].fontIndex);
			b[5] = 0x51; b[6] = 0x4a;
			put_u16(b + 7, (unsigned)runs[idx].fontIndex);
			b[9] = 0x43; b[10] = 0x4a;
			put_u16(b + 11, runs[idx].hps);
		}
		base[0x1ff] = (unsigned char)cnt;
	}
	return fk;
}

/*
 * Build the WordDocument stream from the text and the character FKP pages.
 * The paragraph FKP is a single page (consecutive runs of identical paragraph
 * formatting coalesce, and plain input has one), and the third FKP page is the
 * constant descriptor.  The FIB is patched with the character count and the
 * 1Table geometry, which depends on the number of character FKP pages.
 */
static unsigned char *build_word(const uint16_t *units, size_t ccp,
    size_t fontTableLen, const unsigned char *cfkp, size_t nch,
    size_t *outLen)
{
	size_t text_end = 0x600 + 2 * ccp;
	size_t b0 = align_up(text_end, 0x200);
	size_t npap = 1;
	size_t cbmac = b0 + (nch + npap + 1) * 0x200;
	size_t lVar3 = 0xba + 8 * nch;
	size_t lVar6 = lVar3 + (8 * npap + 4);
	size_t lVar4 = lVar6 + 0x21;
	size_t lVar7 = lVar4 + fontTableLen;

	unsigned char *wd = calloc(1, cbmac);
	if (!wd)
		return NULL;

	memcpy(wd, doc_head_0600, HEAD_LEN);
	put_u32(wd + 0x1c, (uint32_t)(0x600 + 2 * ccp));
	put_u32(wd + 0x40, (uint32_t)cbmac);
	put_u32(wd + 0x4c, (uint32_t)ccp);
	put_u32(wd + 0xfe, (uint32_t)(8 * nch + 4));
	put_u32(wd + 0x102, (uint32_t)lVar3);
	put_u32(wd + 0x106, (uint32_t)(8 * npap + 4));
	put_u32(wd + 0x112, (uint32_t)lVar4);
	put_u32(wd + 0x116, (uint32_t)fontTableLen);
	put_u32(wd + 0x192, (uint32_t)lVar7);
	put_u32(wd + 0x1a2, (uint32_t)(lVar6 + 0xc));
	put_u32(wd + 0x34a, (uint32_t)lVar6);
	for (size_t i = 0; i < ccp; i++)
		put_u16(wd + 0x600 + 2 * i, units[i]);

	memcpy(wd + b0, cfkp, nch * FKP_LEN);

	unsigned char *pfkp = wd + b0 + nch * FKP_LEN;
	memcpy(pfkp, doc_para_fkp_0200, FKP_LEN);
	put_u32(pfkp, 0x600);
	put_u32(pfkp + 4, (uint32_t)text_end);

	unsigned char *dfkp = wd + b0 + (nch + npap) * FKP_LEN;
	memcpy(dfkp, doc_desc_0200, FKP_LEN);
	put_u32(dfkp, 0x600);
	put_u32(dfkp + 4, (uint32_t)text_end);

	*outLen = cbmac;
	return wd;
}

/*
 * Build the 1Table stream.  For an empty document the constant empty template
 * is used; otherwise the constant prefix is patched with the character count
 * and the three Plcfbte / CLX / font-table / doc-info blocks are laid out at
 * offsets derived from the character-count and the number of FKP pages.
 */
static unsigned char *build_table(size_t ccp, int isEmpty,
    const unsigned char *fontTable, size_t fontTableLen, size_t nch,
    const size_t *charEndCp, size_t *outLen)
{
	unsigned char *t = malloc(TABLE_LEN);
	if (!t)
		return NULL;

	if (isEmpty) {
		memcpy(t, table1_empty_4096, TABLE_LEN);
		*outLen = TABLE_LEN;
		return t;
	}

	size_t text_end = 0x600 + 2 * ccp;
	size_t npap = 1;
	size_t uVar37 = align_up(text_end, 0x200) / 0x200;
	size_t uVar41 = nch + uVar37;
	size_t lVar3 = 0xba + 8 * nch;
	size_t lVar6 = lVar3 + (8 * npap + 4);
	size_t lVar4 = lVar6 + 0x21;
	size_t lVar7 = lVar4 + fontTableLen;

	memset(t, 0, TABLE_LEN);
	memcpy(t, table1_nonempty_4096, 0xB6);
	put_u32(t + 0xA6, (uint32_t)ccp);

	/* PlcfbteChpx: CP0, one CP per page, then the page numbers */
	put_u32(t + 0xB6, 0x600);
	for (size_t i = 0; i < nch; i++)
		put_u32(t + 0xBA + 4 * i, (uint32_t)charEndCp[i]);
	for (size_t i = 0; i < nch; i++)
		put_u32(t + 0xBA + 4 * nch + 4 * i, (uint32_t)(uVar37 + i));

	/* PlcfbtePapx: one entry (a single paragraph-formatting run) */
	put_u32(t + lVar3, 0x600);
	put_u32(t + lVar3 + 4, (uint32_t)text_end);
	put_u32(t + lVar3 + 8, (uint32_t)uVar41);

	/* third Plcfbte: fcPlcfbtePgd? the descriptor page */
	put_u32(t + lVar6, 0x600);
	put_u32(t + lVar6 + 4, (uint32_t)text_end);
	put_u32(t + lVar6 + 8, (uint32_t)(uVar41 + npap));

	/* CLX (constant, with CP1 = ccp) */
	memcpy(t + lVar6 + 0xc, table1_nonempty_4096 + 0xDA, 0x15);
	put_u32(t + lVar6 + 0xc + 9, (uint32_t)ccp);

	memcpy(t + lVar4, fontTable, fontTableLen);
	memcpy(t + lVar7, table1_nonempty_4096 + DOCINFO_OFF, DOCINFO_LEN);
	if (lVar7 + DOCINFO_LEN < TABLE_LEN)
		memset(t + lVar7 + DOCINFO_LEN, 0, TABLE_LEN - lVar7 - DOCINFO_LEN);

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

/* ---- public entry points ------------------------------------------------ */

int UIDocFormatWriterGenerateRuns(const char *utf8, size_t len,
    const UIDocFormatWriterRun *runs, size_t runCount,
    unsigned char **out, size_t *outLen)
{
	int isEmpty = 0;
	size_t ccp = 0;
	uint16_t *units = prepare_text(utf8, len, &ccp, &isEmpty);
	if (!units)
		return -1;

	body_run *br = NULL;
	size_t nruns = 0;
	char **names = NULL;
	size_t nfonts = 0;
	unsigned char *fontTable = NULL;
	size_t fontTableLen = 0;
	size_t *run_page = NULL, *run_off = NULL, *page_end_cp = NULL;
	unsigned char *cfkp = NULL;
	size_t nch = 0;

	if (isEmpty) {
		/*
		 * Empty documents ignore any requested styling and use the
		 * empty template, whose font table is 12 bytes shorter (its
		 * body font is "Helvetica", not "Helvetica Light").
		 */
		fontTableLen = 0xf2;
		br = malloc(sizeof(body_run));
		if (!br) { free(units); return -1; }
		br[0] = (body_run){ 0, 3, 24 };
		nruns = 1;
	} else {
		UIDocFormatWriterRun defrun = { 0, (size_t)-1, NULL, 0 };
		const UIDocFormatWriterRun *er = runs;
		size_t ne = runCount;
		int *idx;

		if (!er || ne == 0) { er = &defrun; ne = 1; }

		idx = malloc(ne * sizeof(int));
		if (!idx) { free(units); return -1; }
		names = collect_fonts(er, ne, idx, &nfonts);
		fontTable = names ? build_font_table(names, nfonts,
		    &fontTableLen) : NULL;
		if (!fontTable) {
			free(idx); free(names); free(units);
			return -1;
		}

		size_t bodyLen = ccp - 1;
		br = malloc(ne * sizeof(body_run));
		if (!br) {
			free(idx); free(fontTable); free(names); free(units);
			return -1;
		}
		size_t prev = 0;
		for (size_t i = 0; i < ne; i++) {
			size_t st = er[i].characterIndex;
			double pt;
			if (st > bodyLen)
				st = bodyLen;
			if (nruns > 0 && st <= prev)
				continue;
			pt = er[i].pointSize > 0 ? er[i].pointSize : 12.0;
			br[nruns].start = st;
			br[nruns].fontIndex = idx[i];
			br[nruns].hps = (uint16_t)(pt * 2.0 + 0.01);
			prev = st;
			nruns++;
		}
		free(idx);
		br[0].start = 0;
	}

	run_page = malloc(nruns * sizeof(size_t));
	run_off = malloc(nruns * sizeof(size_t));
	if (!run_page || !run_off ||
	    plan_char_fkp(br, nruns, ccp, run_page, run_off, &page_end_cp,
	    &nch) != 0) {
		free(units); free(br); free(names); free(fontTable);
		free(run_page); free(run_off);
		return -1;
	}
	cfkp = render_char_fkp(br, nruns, run_page, run_off, page_end_cp, nch);
	free(run_page); free(run_off); free(br);
	if (!cfkp) {
		free(units); free(names); free(fontTable); free(page_end_cp);
		return -1;
	}

	size_t wdLen = 0, t1Len = 0;
	unsigned char *wd = build_word(units, ccp, fontTableLen, cfkp, nch,
	    &wdLen);
	unsigned char *t1 = build_table(ccp, isEmpty, fontTable, fontTableLen,
	    nch, page_end_cp, &t1Len);
	free(units); free(cfkp); free(page_end_cp); free(names); free(fontTable);

	unsigned char *si = malloc(TABLE_LEN);
	unsigned char *dsi = malloc(TABLE_LEN);
	if (!wd || !t1 || !si || !dsi) {
		free(wd); free(t1); free(si); free(dsi);
		return -1;
	}
	memcpy(si, si_4096, TABLE_LEN);
	memcpy(dsi, dsi_4096, TABLE_LEN);
	size_t siLen = TABLE_LEN, dsiLen = TABLE_LEN;

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

int UIDocFormatWriterGenerateStyled(const char *utf8, size_t len,
    const UIDocFormatWriterStyle *style, unsigned char **out, size_t *outLen)
{
	UIDocFormatWriterRun run = {
		0, (size_t)-1,
		style ? style->fontName : NULL,
		style ? style->pointSize : 0.0
	};
	return UIDocFormatWriterGenerateRuns(utf8, len, &run, 1, out, outLen);
}

int UIDocFormatWriterGenerate(const char *utf8, size_t len,
    unsigned char **out, size_t *outLen)
{
	return UIDocFormatWriterGenerateStyled(utf8, len, NULL, out, outLen);
}
