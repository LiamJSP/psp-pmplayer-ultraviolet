// subtitle_text.c
#include "subtitle_text.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef SUBTITLE_TEXT_MAX_SOURCE
#define SUBTITLE_TEXT_MAX_SOURCE 4096
#endif

#ifndef SUBTITLE_TEXT_MAX_CODEPOINTS
#define SUBTITLE_TEXT_MAX_CODEPOINTS 1024
#endif

#ifndef SUBTITLE_TEXT_WRAP_UNITS
#define SUBTITLE_TEXT_WRAP_UNITS 54
#endif

typedef struct {
	unsigned long base;
	unsigned long isolated;
	unsigned long final;
	unsigned long initial;
	unsigned long medial;
} subtitle_arabic_form_t;

static const subtitle_arabic_form_t g_arabic_forms[] = {
	{0x0621,0xFE80,0,0,0},
	{0x0622,0xFE81,0xFE82,0,0},
	{0x0623,0xFE83,0xFE84,0,0},
	{0x0624,0xFE85,0xFE86,0,0},
	{0x0625,0xFE87,0xFE88,0,0},
	{0x0626,0xFE89,0xFE8A,0xFE8B,0xFE8C},
	{0x0627,0xFE8D,0xFE8E,0,0},
	{0x0628,0xFE8F,0xFE90,0xFE91,0xFE92},
	{0x0629,0xFE93,0xFE94,0,0},
	{0x062A,0xFE95,0xFE96,0xFE97,0xFE98},
	{0x062B,0xFE99,0xFE9A,0xFE9B,0xFE9C},
	{0x062C,0xFE9D,0xFE9E,0xFE9F,0xFEA0},
	{0x062D,0xFEA1,0xFEA2,0xFEA3,0xFEA4},
	{0x062E,0xFEA5,0xFEA6,0xFEA7,0xFEA8},
	{0x062F,0xFEA9,0xFEAA,0,0},
	{0x0630,0xFEAB,0xFEAC,0,0},
	{0x0631,0xFEAD,0xFEAE,0,0},
	{0x0632,0xFEAF,0xFEB0,0,0},
	{0x0633,0xFEB1,0xFEB2,0xFEB3,0xFEB4},
	{0x0634,0xFEB5,0xFEB6,0xFEB7,0xFEB8},
	{0x0635,0xFEB9,0xFEBA,0xFEBB,0xFEBC},
	{0x0636,0xFEBD,0xFEBE,0xFEBF,0xFEC0},
	{0x0637,0xFEC1,0xFEC2,0xFEC3,0xFEC4},
	{0x0638,0xFEC5,0xFEC6,0xFEC7,0xFEC8},
	{0x0639,0xFEC9,0xFECA,0xFECB,0xFECC},
	{0x063A,0xFECD,0xFECE,0xFECF,0xFED0},
	{0x0641,0xFED1,0xFED2,0xFED3,0xFED4},
	{0x0642,0xFED5,0xFED6,0xFED7,0xFED8},
	{0x0643,0xFED9,0xFEDA,0xFEDB,0xFEDC},
	{0x0644,0xFEDD,0xFEDE,0xFEDF,0xFEE0},
	{0x0645,0xFEE1,0xFEE2,0xFEE3,0xFEE4},
	{0x0646,0xFEE5,0xFEE6,0xFEE7,0xFEE8},
	{0x0647,0xFEE9,0xFEEA,0xFEEB,0xFEEC},
	{0x0648,0xFEED,0xFEEE,0,0},
	{0x0649,0xFEEF,0xFEF0,0,0},
	{0x064A,0xFEF1,0xFEF2,0xFEF3,0xFEF4}
};

static int subtitle_text_ascii_tolower(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c + ('a' - 'A');

	return c;
}

static int subtitle_text_ascii_ieq(const char *a, const char *b)
{
	if (a == 0 || b == 0)
		return 0;

	while (*a && *b) {
		if (subtitle_text_ascii_tolower((unsigned char)*a) !=
		    subtitle_text_ascii_tolower((unsigned char)*b))
			return 0;

		a++;
		b++;
	}

	return *a == 0 && *b == 0;
}

static int subtitle_text_ascii_istarts(const char *s, const char *prefix)
{
	if (s == 0 || prefix == 0)
		return 0;

	while (*prefix) {
		if (*s == 0)
			return 0;

		if (subtitle_text_ascii_tolower((unsigned char)*s) !=
		    subtitle_text_ascii_tolower((unsigned char)*prefix))
			return 0;

		s++;
		prefix++;
	}

	return 1;
}

static void subtitle_text_trim_field(char *s)
{
	char *end;

	if (s == 0)
		return;

	while (*s && isspace((unsigned char)*s))
		memmove(s, s + 1, strlen(s));

	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) {
		end--;
		*end = '\0';
	}
}

static void subtitle_text_append_byte(char *dst,
                                      size_t dst_size,
                                      size_t *pos,
                                      char c)
{
	if (dst == 0 || pos == 0 || dst_size == 0)
		return;

	if (*pos + 1 >= dst_size)
		return;

	dst[*pos] = c;
	*pos += 1;
	dst[*pos] = '\0';
}

static void subtitle_text_append_utf8(char *dst,
                                      size_t dst_size,
                                      size_t *pos,
                                      unsigned long cp)
{
	if (cp == 0)
		return;

	if (cp > 0x10FFFFUL)
		cp = '?';

	if (cp >= 0xD800UL && cp <= 0xDFFFUL)
		cp = '?';

	if (cp < 0x80UL) {
		subtitle_text_append_byte(dst, dst_size, pos, (char)cp);
	}
	else if (cp < 0x800UL) {
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0xC0 | (cp >> 6)));
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0x80 | (cp & 0x3F)));
	}
	else if (cp < 0x10000UL) {
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0xE0 | (cp >> 12)));
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0x80 | ((cp >> 6) & 0x3F)));
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0x80 | (cp & 0x3F)));
	}
	else {
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0xF0 | (cp >> 18)));
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0x80 | ((cp >> 12) & 0x3F)));
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0x80 | ((cp >> 6) & 0x3F)));
		subtitle_text_append_byte(dst, dst_size, pos,
		                          (char)(0x80 | (cp & 0x3F)));
	}
}

static unsigned long subtitle_text_decode_utf8_one(const char **ps)
{
	const unsigned char *p;
	unsigned long cp;

	if (ps == 0 || *ps == 0)
		return 0;

	p = (const unsigned char *)(*ps);

	while ((*p & 0xC0) == 0x80)
		p++;

	if (*p == 0) {
		*ps = (const char *)p;
		return 0;
	}

	if ((*p & 0x80) == 0) {
		cp = *p;
		*ps = (const char *)(p + 1);
		return cp;
	}

	if ((*p & 0xE0) == 0xC0) {
		if ((p[1] & 0xC0) != 0x80) {
			*ps = (const char *)(p + 1);
			return '?';
		}

		cp = ((unsigned long)(p[0] & 0x1F) << 6) |
		     ((unsigned long)(p[1] & 0x3F));

		if (cp < 0x80)
			cp = '?';

		*ps = (const char *)(p + 2);
		return cp;
	}

	if ((*p & 0xF0) == 0xE0) {
		if ((p[1] & 0xC0) != 0x80 ||
		    (p[2] & 0xC0) != 0x80) {
			*ps = (const char *)(p + 1);
			return '?';
		}

		cp = ((unsigned long)(p[0] & 0x0F) << 12) |
		     ((unsigned long)(p[1] & 0x3F) << 6) |
		     ((unsigned long)(p[2] & 0x3F));

		if (cp < 0x800 ||
		    (cp >= 0xD800 && cp <= 0xDFFF))
			cp = '?';

		*ps = (const char *)(p + 3);
		return cp;
	}

	if ((*p & 0xF8) == 0xF0) {
		if ((p[1] & 0xC0) != 0x80 ||
		    (p[2] & 0xC0) != 0x80 ||
		    (p[3] & 0xC0) != 0x80) {
			*ps = (const char *)(p + 1);
			return '?';
		}

		cp = ((unsigned long)(p[0] & 0x07) << 18) |
		     ((unsigned long)(p[1] & 0x3F) << 12) |
		     ((unsigned long)(p[2] & 0x3F) << 6) |
		     ((unsigned long)(p[3] & 0x3F));

		if (cp < 0x10000 || cp > 0x10FFFF)
			cp = '?';

		*ps = (const char *)(p + 4);
		return cp;
	}

	*ps = (const char *)(p + 1);
	return '?';
}

static int subtitle_text_hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';

	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;

	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

static int subtitle_text_is_format_control(unsigned long cp)
{
	/*
	 * Do not render Unicode formatting controls as visible glyphs.
	 *
	 * These are useful in rich text engines, but on PSP subtitles they
	 * commonly arrive as leading &lrm; / &rlm; markers and should not
	 * become visible boxes or literal text.
	 */
	if (cp == 0x00AD) /* soft hyphen */
		return 1;

	if (cp == 0x061C) /* Arabic Letter Mark */
		return 1;

	if (cp == 0x200B) /* zero width space */
		return 1;

	if (cp == 0x200C) /* zero width non-joiner */
		return 0;

	if (cp == 0x200D) /* zero width joiner */
		return 0;

	if (cp == 0x200E || cp == 0x200F) /* LRM / RLM */
		return 1;

	if (cp >= 0x202A && cp <= 0x202E) /* bidi embedding/override */
		return 1;

	if (cp >= 0x2066 && cp <= 0x2069) /* bidi isolates */
		return 1;

	if (cp == 0xFEFF) /* BOM / zero width no-break space */
		return 1;

	return 0;
}

static int subtitle_text_decode_entity(const char *src,
                                       size_t src_len,
                                       size_t *advance,
                                       unsigned long *out_cp)
{
	size_t i;
	unsigned long value = 0;
	int hex = 0;

	if (advance == 0 || out_cp == 0)
		return 0;

	*advance = 0;
	*out_cp = 0;

	if (src == 0 || src_len < 3 || src[0] != '&')
		return 0;

	for (i = 1; i < src_len && i < 16; i++) {
		if (src[i] == ';')
			break;
	}

	if (i >= src_len || i >= 16 || src[i] != ';')
		return 0;

	if (i == 3 && subtitle_text_ascii_istarts(src, "&lt;")) {
		*out_cp = '<';
		*advance = 4;
		return 1;
	}

	if (i == 3 && subtitle_text_ascii_istarts(src, "&gt;")) {
		*out_cp = '>';
		*advance = 4;
		return 1;
	}

	if (i == 4 && subtitle_text_ascii_istarts(src, "&amp;")) {
		*out_cp = '&';
		*advance = 5;
		return 1;
	}

	if (i == 5 && subtitle_text_ascii_istarts(src, "&quot;")) {
		*out_cp = '"';
		*advance = 6;
		return 1;
	}

	if (i == 5 && subtitle_text_ascii_istarts(src, "&nbsp;")) {
		*out_cp = ' ';
		*advance = 6;
		return 1;
	}

	if (i == 6 && subtitle_text_ascii_istarts(src, "&apos;")) {
		*out_cp = '\'';
		*advance = 7;
		return 1;
	}

	if (i == 4 && subtitle_text_ascii_istarts(src, "&lrm;")) {
		*out_cp = 0x200E;
		*advance = 5;
		return 1;
	}

	if (i == 4 && subtitle_text_ascii_istarts(src, "&rlm;")) {
		*out_cp = 0x200F;
		*advance = 5;
		return 1;
	}

	if (i == 4 && subtitle_text_ascii_istarts(src, "&zwj;")) {
		*out_cp = 0x200D;
		*advance = 5;
		return 1;
	}

	if (i == 5 && subtitle_text_ascii_istarts(src, "&zwnj;")) {
		*out_cp = 0x200C;
		*advance = 6;
		return 1;
	}

	if (i == 4 && subtitle_text_ascii_istarts(src, "&shy;")) {
		*out_cp = 0x00AD;
		*advance = 5;
		return 1;
	}

	if (i == 5 && subtitle_text_ascii_istarts(src, "&ndash;")) {
		*out_cp = 0x2013;
		*advance = 7;
		return 1;
	}

	if (i == 5 && subtitle_text_ascii_istarts(src, "&mdash;")) {
		*out_cp = 0x2014;
		*advance = 7;
		return 1;
	}

	if (src[1] != '#')
		return 0;

	i = 2;

	if (i < src_len && (src[i] == 'x' || src[i] == 'X')) {
		hex = 1;
		i++;
	}

	if (i >= src_len)
		return 0;

	while (i < src_len && src[i] != ';') {
		int digit;

		if (hex) {
			digit = subtitle_text_hex_value(src[i]);
			if (digit < 0)
				return 0;

			if (value > 0x10FFFFUL / 16UL)
				return 0;

			value = value * 16UL + (unsigned long)digit;
		}
		else {
			if (src[i] < '0' || src[i] > '9')
				return 0;

			if (value > 0x10FFFFUL / 10UL)
				return 0;

			value = value * 10UL + (unsigned long)(src[i] - '0');
		}

		i++;
	}

	if (i >= src_len || src[i] != ';')
		return 0;

	*out_cp = value;
	*advance = i + 1;
	return 1;
}

static const char *subtitle_text_skip_utf8_bom(const char *s)
{
	if (s == 0)
		return s;

	if ((unsigned char)s[0] == 0xEF &&
	    (unsigned char)s[1] == 0xBB &&
	    (unsigned char)s[2] == 0xBF)
		return s + 3;

	return s;
}

static int subtitle_text_extract_after_commas(const char *src,
                                              unsigned int commas,
                                              const char **out)
{
	unsigned int seen = 0;
	const char *p;

	if (src == 0 || out == 0)
		return 0;

	p = src;

	while (*p && seen < commas) {
		if (*p == ',')
			seen++;

		p++;
	}

	if (seen != commas)
		return 0;

	*out = p;
	return 1;
}

static int subtitle_text_private_dialogue_text_index(const uint8_t *codec_private,
                                                     uint32_t codec_private_size)
{
	char buf[SUBTITLE_TEXT_MAX_SOURCE + 1];
	uint32_t n;
	char *line;
	char *save;
	int in_events = 0;

	if (codec_private == 0 || codec_private_size == 0)
		return -1;

	n = codec_private_size;
	if (n > SUBTITLE_TEXT_MAX_SOURCE)
		n = SUBTITLE_TEXT_MAX_SOURCE;

	memcpy(buf, codec_private, n);
	buf[n] = '\0';

	line = strtok_r(buf, "\r\n", &save);
	while (line != 0) {
		while (*line && isspace((unsigned char)*line))
			line++;

		if (*line == '[') {
			in_events = subtitle_text_ascii_istarts(line, "[Events]");
		}
		else if (in_events && subtitle_text_ascii_istarts(line, "Format:")) {
			char *fields;
			char *field;
			char *save2;
			int index = 0;

			fields = strchr(line, ':');
			if (fields == 0)
				return -1;

			fields++;

			field = strtok_r(fields, ",", &save2);
			while (field != 0) {
				subtitle_text_trim_field(field);

				if (subtitle_text_ascii_ieq(field, "Text"))
					return index;

				index++;
				field = strtok_r(0, ",", &save2);
			}
		}

		line = strtok_r(0, "\r\n", &save);
	}

	return -1;
}

static const char *subtitle_text_extract_ass_text(const char *src,
                                                  const uint8_t *codec_private,
                                                  uint32_t codec_private_size)
{
	const char *p;
	const char *text;
	int text_index;

	if (src == 0)
		return "";

	p = src;

	while (*p && isspace((unsigned char)*p))
		p++;

	if (subtitle_text_ascii_istarts(p, "Dialogue:")) {
		p = strchr(p, ':');
		if (p != 0)
			p++;

		while (*p && isspace((unsigned char)*p))
			p++;

		text_index =
			subtitle_text_private_dialogue_text_index(codec_private,
			                                          codec_private_size);

		if (text_index > 0 &&
		    subtitle_text_extract_after_commas(p, (unsigned int)text_index, &text))
			return text;

		if (subtitle_text_extract_after_commas(p, 9, &text))
			return text;

		return p;
	}

	if (subtitle_text_extract_after_commas(p, 8, &text))
		return text;

	return p;
}

static int subtitle_text_is_cjk(unsigned long cp)
{
	if (cp >= 0x1100 && cp <= 0x11FF)
		return 1;

	if (cp >= 0x2E80 && cp <= 0x9FFF)
		return 1;

	if (cp >= 0xAC00 && cp <= 0xD7AF)
		return 1;

	if (cp >= 0xF900 && cp <= 0xFAFF)
		return 1;

	if (cp >= 0x20000 && cp <= 0x2FA1F)
		return 1;

	if (cp >= 0x3040 && cp <= 0x30FF)
		return 1;

	if (cp >= 0x31F0 && cp <= 0x31FF)
		return 1;

	if (cp >= 0xFF00 && cp <= 0xFFEF)
		return 1;

	return 0;
}

static int subtitle_text_is_rtl(unsigned long cp)
{
	if (cp >= 0x0590 && cp <= 0x08FF)
		return 1;

	if (cp >= 0xFB1D && cp <= 0xFDFF)
		return 1;

	if (cp >= 0xFE70 && cp <= 0xFEFF)
		return 1;

	return 0;
}

static int subtitle_text_is_arabic(unsigned long cp)
{
	if (cp >= 0x0600 && cp <= 0x06FF)
		return 1;

	if (cp >= 0x0750 && cp <= 0x077F)
		return 1;

	if (cp >= 0x08A0 && cp <= 0x08FF)
		return 1;

	return 0;
}

static int subtitle_text_is_complex_unshaped(unsigned long cp)
{
	if (cp >= 0x0900 && cp <= 0x0DFF)
		return 1;

	if (cp >= 0x0E00 && cp <= 0x0E7F)
		return 1;

	if (cp >= 0x1780 && cp <= 0x17FF)
		return 1;

	if (cp >= 0x1000 && cp <= 0x109F)
		return 1;

	return 0;
}

static int subtitle_text_is_arabic_transparent(unsigned long cp)
{
	if (cp >= 0x064B && cp <= 0x065F)
		return 1;

	if (cp == 0x0670)
		return 1;

	if (cp >= 0x06D6 && cp <= 0x06ED)
		return 1;

	return 0;
}

static const subtitle_arabic_form_t *subtitle_text_arabic_form(unsigned long cp)
{
	unsigned int i;

	for (i = 0; i < sizeof(g_arabic_forms) / sizeof(g_arabic_forms[0]); i++) {
		if (g_arabic_forms[i].base == cp)
			return &g_arabic_forms[i];
	}

	return 0;
}

static int subtitle_text_arabic_can_connect_prev(unsigned long cp)
{
	const subtitle_arabic_form_t *f = subtitle_text_arabic_form(cp);

	return f != 0 && f->final != 0;
}

static int subtitle_text_arabic_can_connect_next(unsigned long cp)
{
	const subtitle_arabic_form_t *f = subtitle_text_arabic_form(cp);

	return f != 0 && f->initial != 0;
}

static int subtitle_text_prev_join_index(const unsigned long *cps, int index)
{
	int i;

	for (i = index - 1; i >= 0; i--) {
		if (subtitle_text_is_arabic_transparent(cps[i]))
			continue;

		return i;
	}

	return -1;
}

static int subtitle_text_next_join_index(const unsigned long *cps, int count, int index)
{
	int i;

	for (i = index + 1; i < count; i++) {
		if (subtitle_text_is_arabic_transparent(cps[i]))
			continue;

		return i;
	}

	return -1;
}

static int subtitle_text_is_alef_variant(unsigned long cp)
{
	return cp == 0x0622 || cp == 0x0623 ||
	       cp == 0x0625 || cp == 0x0627;
}

static unsigned long subtitle_text_lamalef_form(unsigned long alef, int connects_prev)
{
	switch (alef) {
		case 0x0622:
			return connects_prev ? 0xFEF6 : 0xFEF5;

		case 0x0623:
			return connects_prev ? 0xFEF8 : 0xFEF7;

		case 0x0625:
			return connects_prev ? 0xFEFA : 0xFEF9;

		case 0x0627:
		default:
			return connects_prev ? 0xFEFC : 0xFEFB;
	}
}

static int subtitle_text_shape_arabic(const unsigned long *in,
                                      int in_count,
                                      unsigned long *out,
                                      int out_max)
{
	int i;
	int out_count = 0;

	for (i = 0; i < in_count && out_count < out_max; i++) {
		unsigned long cp = in[i];
		const subtitle_arabic_form_t *f;
		int pi;
		int ni;
		int connects_prev = 0;
		int connects_next = 0;

		if (cp == 0x0644 &&
		    i + 1 < in_count &&
		    subtitle_text_is_alef_variant(in[i + 1])) {
			pi = subtitle_text_prev_join_index(in, i);

			if (pi >= 0 &&
			    subtitle_text_arabic_can_connect_next(in[pi]) &&
			    subtitle_text_arabic_can_connect_prev(cp))
				connects_prev = 1;

			out[out_count++] =
				subtitle_text_lamalef_form(in[i + 1], connects_prev);

			i++;
			continue;
		}

		f = subtitle_text_arabic_form(cp);
		if (f == 0) {
			out[out_count++] = cp;
			continue;
		}

		pi = subtitle_text_prev_join_index(in, i);
		ni = subtitle_text_next_join_index(in, in_count, i);

		if (pi >= 0 &&
		    subtitle_text_arabic_can_connect_next(in[pi]) &&
		    subtitle_text_arabic_can_connect_prev(cp))
			connects_prev = 1;

		if (ni >= 0 &&
		    subtitle_text_arabic_can_connect_next(cp) &&
		    subtitle_text_arabic_can_connect_prev(in[ni]))
			connects_next = 1;

		if (connects_prev && connects_next && f->medial)
			out[out_count++] = f->medial;
		else if (connects_prev && f->final)
			out[out_count++] = f->final;
		else if (connects_next && f->initial)
			out[out_count++] = f->initial;
		else
			out[out_count++] = f->isolated;
	}

	return out_count;
}

static unsigned long subtitle_text_mirror_pair(unsigned long cp)
{
	switch (cp) {
		case '(':
			return ')';
		case ')':
			return '(';
		case '[':
			return ']';
		case ']':
			return '[';
		case '{':
			return '}';
		case '}':
			return '{';
		case '<':
			return '>';
		case '>':
			return '<';
		default:
			return cp;
	}
}

static int subtitle_text_decode_line_to_cps(const char *line,
                                            unsigned long *cps,
                                            int max_cps,
                                            unsigned int *flags)
{
	const char *p = line;
	int count = 0;

	while (p != 0 && *p && count < max_cps) {
		unsigned long cp = subtitle_text_decode_utf8_one(&p);

		if (cp == 0)
			break;

		cps[count++] = cp;

		if (flags) {
			if (subtitle_text_is_cjk(cp))
				*flags |= SUBTITLE_TEXT_FLAG_CJK;

			if (subtitle_text_is_rtl(cp))
				*flags |= SUBTITLE_TEXT_FLAG_RTL;

			if (subtitle_text_is_arabic(cp))
				*flags |= SUBTITLE_TEXT_FLAG_ARABIC_SHAPED;

			if (subtitle_text_is_complex_unshaped(cp))
				*flags |= SUBTITLE_TEXT_FLAG_COMPLEX_UNSHAPED;
		}
	}

	return count;
}

static int subtitle_text_first_strong_is_rtl(const unsigned long *cps, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		unsigned long cp = cps[i];

		if (subtitle_text_is_rtl(cp))
			return 1;

		if ((cp >= 'A' && cp <= 'Z') ||
		    (cp >= 'a' && cp <= 'z') ||
		    (cp >= 0x0400 && cp <= 0x052F) ||
		    (cp >= 0x0370 && cp <= 0x03FF))
			return 0;
	}

	return 0;
}

static int subtitle_text_cp_is_ltr_strong(unsigned long cp)
{
	if ((cp >= 'A' && cp <= 'Z') ||
	    (cp >= 'a' && cp <= 'z') ||
	    (cp >= 0x0400 && cp <= 0x052F) ||
	    (cp >= 0x0370 && cp <= 0x03FF))
		return 1;

	return 0;
}

static int subtitle_text_cp_is_neutral(unsigned long cp)
{
	if (cp == ' ' || cp == '\t' ||
	    cp == '.' || cp == ',' || cp == ':' ||
	    cp == ';' || cp == '!' || cp == '?' ||
	    cp == '-' || cp == '_' || cp == '\'' ||
	    cp == '"' || cp == '(' || cp == ')' ||
	    cp == '[' || cp == ']' || cp == '{' ||
	    cp == '}' || cp == '<' || cp == '>' ||
	    cp == '/')
		return 1;

	if (cp >= '0' && cp <= '9')
		return 1;

	return 0;
}

static int subtitle_text_run_dir(const unsigned long *cps, int count, int index, int base_rtl)
{
	unsigned long cp = cps[index];

	if (subtitle_text_is_rtl(cp))
		return 1;

	if (subtitle_text_cp_is_ltr_strong(cp))
		return 0;

	if (subtitle_text_cp_is_neutral(cp))
		return base_rtl ? 1 : 0;

	return 0;
}

static void subtitle_text_append_visual_run(char *dst,
                                            size_t dst_size,
                                            size_t *pos,
                                            const unsigned long *cps,
                                            int start,
                                            int end,
                                            int rtl)
{
	int i;

	if (rtl) {
		for (i = end - 1; i >= start; i--)
			subtitle_text_append_utf8(dst,
			                          dst_size,
			                          pos,
			                          subtitle_text_mirror_pair(cps[i]));
	}
	else {
		for (i = start; i < end; i++)
			subtitle_text_append_utf8(dst, dst_size, pos, cps[i]);
	}
}

static void subtitle_text_bidi_shape_line(char *dst,
                                          size_t dst_size,
                                          const char *line,
                                          unsigned int *flags)
{
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	const char *p;
	size_t pos = 0;

	/*
	 * HarfBuzz/FriBidi renderer path:
	 *
	 * Keep subtitle text in normal Unicode logical order.
	 * Do not pre-shape Arabic into presentation forms.
	 * Do not manually reverse RTL runs here.
	 *
	 * The renderer will do:
	 *   logical Unicode -> FriBidi levels -> HarfBuzz shaping -> FreeType glyphs
	 */

	if (dst_size == 0)
		return;

	dst[0] = '\0';

	if (line == 0)
		return;

	p = line;

	while (*p && pos + 1 < dst_size) {
		const char *before = p;
		unsigned long cp = subtitle_text_decode_utf8_one(&p);

		if (cp == 0)
			break;

		if (flags) {
			if (subtitle_text_is_cjk(cp))
				*flags |= SUBTITLE_TEXT_FLAG_CJK;

			if (subtitle_text_is_rtl(cp))
				*flags |= SUBTITLE_TEXT_FLAG_RTL;

			if (subtitle_text_is_arabic(cp))
				*flags |= SUBTITLE_TEXT_FLAG_ARABIC_SHAPED;

			if (subtitle_text_is_complex_unshaped(cp))
				*flags |= SUBTITLE_TEXT_FLAG_COMPLEX_UNSHAPED;
		}

		if (!subtitle_text_is_format_control(cp))
	        subtitle_text_append_utf8(dst, dst_size, &pos, cp);

		if (before == p)
			break;
	}
#else
	unsigned long cps[SUBTITLE_TEXT_MAX_CODEPOINTS];
	unsigned long shaped[SUBTITLE_TEXT_MAX_CODEPOINTS];
	int count;
	int shaped_count;
	int base_rtl;
	int runs_start[128];
	int runs_end[128];
	int runs_dir[128];
	int run_count = 0;
	int i;
	int start;
	int dir;
	size_t pos = 0;

	if (dst_size == 0)
		return;

	dst[0] = '\0';

	count =
		subtitle_text_decode_line_to_cps(line,
		                                 cps,
		                                 SUBTITLE_TEXT_MAX_CODEPOINTS,
		                                 flags);

	if (count <= 0)
		return;

	shaped_count =
		subtitle_text_shape_arabic(cps,
		                           count,
		                           shaped,
		                           SUBTITLE_TEXT_MAX_CODEPOINTS);

	base_rtl = subtitle_text_first_strong_is_rtl(shaped, shaped_count);

	start = 0;
	dir = subtitle_text_run_dir(shaped, shaped_count, 0, base_rtl);

	for (i = 1; i < shaped_count; i++) {
		int d = subtitle_text_run_dir(shaped, shaped_count, i, base_rtl);

		if (d != dir) {
			if (run_count < 128) {
				runs_start[run_count] = start;
				runs_end[run_count] = i;
				runs_dir[run_count] = dir;
				run_count++;
			}

			start = i;
			dir = d;
		}
	}

	if (run_count < 128) {
		runs_start[run_count] = start;
		runs_end[run_count] = shaped_count;
		runs_dir[run_count] = dir;
		run_count++;
	}

	if (base_rtl) {
		for (i = run_count - 1; i >= 0; i--) {
			subtitle_text_append_visual_run(dst,
			                                dst_size,
			                                &pos,
			                                shaped,
			                                runs_start[i],
			                                runs_end[i],
			                                runs_dir[i]);
		}
	}
	else {
		for (i = 0; i < run_count; i++) {
			subtitle_text_append_visual_run(dst,
			                                dst_size,
			                                &pos,
			                                shaped,
			                                runs_start[i],
			                                runs_end[i],
			                                runs_dir[i]);
		}
	}
#endif
}

static int subtitle_text_char_units(unsigned long cp)
{
	if (cp == '\n')
		return 0;

	if (subtitle_text_is_cjk(cp))
		return 2;

	if (cp >= 0x1100)
		return 2;

	return 1;
}

static void subtitle_text_wrap_visual_utf8(char *dst,
                                           size_t dst_size,
                                           const char *src,
                                           unsigned int *out_lines)
{
	const char *p;
	size_t pos = 0;
	int units = 0;
	unsigned int lines = 1;

	if (dst_size == 0)
		return;

	dst[0] = '\0';

	if (out_lines)
		*out_lines = 0;

	if (src == 0 || src[0] == '\0')
		return;

	p = src;

	while (*p && pos + 1 < dst_size) {
		const char *before = p;
		unsigned long cp = subtitle_text_decode_utf8_one(&p);
		int cu;

		if (cp == 0)
			break;

		if (cp == '\r')
			continue;

		if (cp == '\n') {
			if (pos > 0 && dst[pos - 1] != '\n') {
				subtitle_text_append_byte(dst, dst_size, &pos, '\n');
				lines++;
			}
			units = 0;
			continue;
		}

		cu = subtitle_text_char_units(cp);

		if (units > 0 && units + cu > SUBTITLE_TEXT_WRAP_UNITS) {
			subtitle_text_append_byte(dst, dst_size, &pos, '\n');
			lines++;
			units = 0;

			if (cp == ' ')
				continue;
		}

		if (cp == ' ' && units == 0)
			continue;

		subtitle_text_append_utf8(dst, dst_size, &pos, cp);
		units += cu;

		if (subtitle_text_is_cjk(cp) &&
		    units >= SUBTITLE_TEXT_WRAP_UNITS - 1 &&
		    *p != '\0' &&
		    *p != '\n' &&
		    *p != '\r') {
			subtitle_text_append_byte(dst, dst_size, &pos, '\n');
			lines++;
			units = 0;
		}

		if (before == p)
			break;
	}

	while (pos > 0 &&
	       (dst[pos - 1] == '\n' ||
	        dst[pos - 1] == ' ' ||
	        dst[pos - 1] == '\t')) {
		pos--;
		dst[pos] = '\0';
	}

	if (out_lines) {
		if (pos == 0)
			*out_lines = 0;
		else
			*out_lines = lines;
	}
}

static void subtitle_text_clean_copy(char *dst,
                                     size_t dst_size,
                                     const char *src,
                                     int ass_mode)
{
	size_t pos = 0;
	size_t i = 0;
	size_t src_len;

	if (dst == 0 || dst_size == 0)
		return;

	dst[0] = '\0';

	if (src == 0)
		return;

	src = subtitle_text_skip_utf8_bom(src);
	src_len = strlen(src);

	while (src[i] != '\0' && pos + 1 < dst_size) {
		char c = src[i];

		if (c == '\r' || c == '\n') {
			if (pos > 0 && dst[pos - 1] != '\n')
				subtitle_text_append_byte(dst, dst_size, &pos, '\n');

			i++;
			while (src[i] == '\r' || src[i] == '\n')
				i++;

			continue;
		}

		if (c == '<') {
			i++;
			while (src[i] && src[i] != '>')
				i++;

			if (src[i] == '>')
				i++;

			continue;
		}

		if (c == '{') {
			i++;
			while (src[i] && src[i] != '}')
				i++;

			if (src[i] == '}')
				i++;

			continue;
		}

		if (c == '&') {
			size_t adv;
			unsigned long cp;

            if (subtitle_text_decode_entity(src + i,
                                            src_len - i,
                                            &adv,
                                            &cp)) {
                if (!subtitle_text_is_format_control(cp))
                    subtitle_text_append_utf8(dst, dst_size, &pos, cp);

                i += adv;
                continue;
            }
		}

		if (ass_mode && c == '\\') {
			char n = src[i + 1];

			if (n == 'N' || n == 'n') {
				if (pos > 0 && dst[pos - 1] != '\n')
					subtitle_text_append_byte(dst, dst_size, &pos, '\n');

				i += 2;
				continue;
			}

			if (n == 'h') {
				subtitle_text_append_byte(dst, dst_size, &pos, ' ');
				i += 2;
				continue;
			}

			if (n != '\0') {
				subtitle_text_append_byte(dst, dst_size, &pos, n);
				i += 2;
				continue;
			}
		}

		if ((unsigned char)c >= 0x20 || c == '\t')
			subtitle_text_append_byte(dst, dst_size, &pos, c == '\t' ? ' ' : c);

		i++;
	}

	while (pos > 0 &&
	       (dst[pos - 1] == '\n' ||
	        dst[pos - 1] == ' ' ||
	        dst[pos - 1] == '\t')) {
		pos--;
		dst[pos] = '\0';
	}
}

static void subtitle_text_process_lines(char *dst,
                                        size_t dst_size,
                                        unsigned int *out_lines,
                                        unsigned int *out_flags,
                                        const char *src)
{
	char visual[SUBTITLE_TEXT_MAX_SOURCE + 1];
	char wrapped[SUBTITLE_TEXT_MAX_SOURCE + 1];
	char line[SUBTITLE_TEXT_MAX_SOURCE + 1];
	size_t src_i = 0;
	size_t line_i = 0;
	size_t dst_pos = 0;
	unsigned int total_lines = 0;
	unsigned int flags = 0;

	if (dst_size == 0)
		return;

	dst[0] = '\0';

	if (out_lines)
		*out_lines = 0;

	if (out_flags)
		*out_flags = 0;

	if (src == 0 || src[0] == '\0')
		return;

	while (1) {
		char c = src[src_i];

		if (c != '\0' && c != '\n' && line_i + 1 < sizeof(line)) {
			line[line_i++] = c;
			src_i++;
			continue;
		}

		line[line_i] = '\0';

		if (line_i > 0) {
			unsigned int line_count = 0;

			subtitle_text_bidi_shape_line(visual,
			                              sizeof(visual),
			                              line,
			                              &flags);

			subtitle_text_wrap_visual_utf8(wrapped,
			                               sizeof(wrapped),
			                               visual,
			                               &line_count);

			if (wrapped[0] != '\0') {
				size_t k;

				if (dst_pos > 0 && dst[dst_pos - 1] != '\n')
					subtitle_text_append_byte(dst, dst_size, &dst_pos, '\n');

				for (k = 0; wrapped[k] && dst_pos + 1 < dst_size; k++)
					subtitle_text_append_byte(dst, dst_size, &dst_pos, wrapped[k]);

				total_lines += line_count;
			}
		}

		line_i = 0;

		if (c == '\0')
			break;

		src_i++;
	}

	if (out_lines) {
		if (dst[0] == '\0')
			*out_lines = 0;
		else
			*out_lines = total_lines ? total_lines : 1;
	}

	if (out_flags)
		*out_flags = flags;
}

int subtitle_text_normalize_mkv_payload(char *dst,
                                        size_t dst_size,
                                        unsigned int *out_lines,
                                        unsigned int *out_flags,
                                        const uint8_t *payload,
                                        uint64_t payload_size,
                                        uint32_t subtitle_type,
                                        const uint8_t *codec_private,
                                        uint32_t codec_private_size)
{
	char source[SUBTITLE_TEXT_MAX_SOURCE + 1];
	char cleaned[SUBTITLE_TEXT_MAX_SOURCE + 1];
	uint64_t copy_size;
	const char *text;

	if (dst == 0 || dst_size == 0 || payload == 0) {
		if (dst && dst_size)
			dst[0] = '\0';

		if (out_lines)
			*out_lines = 0;

		if (out_flags)
			*out_flags = 0;

		return 0;
	}

	dst[0] = '\0';

	if (out_lines)
		*out_lines = 0;

	if (out_flags)
		*out_flags = 0;

	if (payload_size == 0)
		return 0;

	copy_size = payload_size;
	if (copy_size > SUBTITLE_TEXT_MAX_SOURCE)
		copy_size = SUBTITLE_TEXT_MAX_SOURCE;

	memcpy(source, payload, (size_t)copy_size);
	source[copy_size] = '\0';

	if (subtitle_type == SUBTITLE_TEXT_TYPE_ASS ||
	    subtitle_type == SUBTITLE_TEXT_TYPE_SSA) {
		text = subtitle_text_extract_ass_text(source,
		                                      codec_private,
		                                      codec_private_size);

		subtitle_text_clean_copy(cleaned,
		                         sizeof(cleaned),
		                         text,
		                         1);
	}
	else {
		subtitle_text_clean_copy(cleaned,
		                         sizeof(cleaned),
		                         source,
		                         0);
	}

	subtitle_text_process_lines(dst,
	                            dst_size,
	                            out_lines,
	                            out_flags,
	                            cleaned);

	return dst[0] != '\0';
}