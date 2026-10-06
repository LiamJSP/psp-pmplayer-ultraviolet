#include <stdio.h>
#include <string.h>
#include "subtitle_preferences.h"

typedef struct {
	const char *code1;
	const char *code2;
	const char *code3;
} subtitle_language_equiv_t;

static const subtitle_language_equiv_t subtitle_language_equivs[] = {
	{"en", "eng", 0},
	{"es", "spa", 0},
	{"fr", "fre", "fra"},
	{"de", "ger", "deu"},
	{"it", "ita", 0},
	{"pt", "por", 0},
	{"pl", "pol", 0},
	{"tr", "tur", 0},
	{"sv", "swe", 0},
	{"da", "dan", 0},
	{"fi", "fin", 0},
	{"nl", "dut", "nld"},
	{"no", "nor", 0},
	{"ru", "rus", 0},
	{"el", "gre", "ell"},
	{"he", "heb", 0},
	{"ar", "ara", 0},
	{"ro", "rum", "ron"},
	{"id", "ind", 0},
	{"vi", "vie", 0},
	{"th", "tha", 0},
	{"ko", "kor", 0},
	{"zh", "chi", "zho"},
	{"ja", "jpn", 0},
	{"hi", "hin", 0},
	{"bn", "ben", 0},
	{"ur", "urd", 0},
	{0, 0, 0}
};

static char subtitle_preferred_language[16] = "en";

static int subtitle_pref_ascii_tolower(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c + ('a' - 'A');

	return c;
}

static void subtitle_pref_normalize_language(char *dst,
                                             int dst_size,
                                             const char *src)
{
	int i = 0;
	int j = 0;

	if (dst == 0 || dst_size <= 0)
		return;

	dst[0] = '\0';

	if (src == 0)
		return;

	while (src[i] == ' ' || src[i] == '\t')
		i++;

	while (src[i] != '\0' && j + 1 < dst_size) {
		char ch = src[i];

		if (ch == '-' || ch == '_' || ch == '.' ||
		    ch == ' ' || ch == '\t')
			break;

		if (ch >= 'A' && ch <= 'Z')
			ch = (char)subtitle_pref_ascii_tolower(ch);

		dst[j++] = ch;
		i++;
	}

	dst[j] = '\0';
}

static int subtitle_pref_code_eq(const char *a, const char *b)
{
	char na[16];
	char nb[16];

	subtitle_pref_normalize_language(na, sizeof(na), a);
	subtitle_pref_normalize_language(nb, sizeof(nb), b);

	if (na[0] == '\0' || nb[0] == '\0')
		return 0;

	return strcmp(na, nb) == 0;
}

static int subtitle_pref_code_in_group(const subtitle_language_equiv_t *g,
                                       const char *code)
{
	if (g == 0 || code == 0 || code[0] == '\0')
		return 0;

	if (g->code1 != 0 && subtitle_pref_code_eq(g->code1, code))
		return 1;

	if (g->code2 != 0 && subtitle_pref_code_eq(g->code2, code))
		return 1;

	if (g->code3 != 0 && subtitle_pref_code_eq(g->code3, code))
		return 1;

	return 0;
}

static const subtitle_language_equiv_t *subtitle_pref_find_group(const char *code)
{
	int i;

	if (code == 0 || code[0] == '\0')
		return 0;

	for (i = 0; subtitle_language_equivs[i].code1 != 0; i++) {
		if (subtitle_pref_code_in_group(&subtitle_language_equivs[i], code))
			return &subtitle_language_equivs[i];
	}

	return 0;
}

static int subtitle_pref_equivalent(const char *a, const char *b)
{
	const subtitle_language_equiv_t *ga;
	const subtitle_language_equiv_t *gb;

	if (subtitle_pref_code_eq(a, b))
		return 1;

	ga = subtitle_pref_find_group(a);
	gb = subtitle_pref_find_group(b);

	if (ga != 0 && gb != 0 && ga == gb)
		return 1;

	return 0;
}

void subtitle_preferences_set_preferred_language(const char *language)
{
	char normalized[16];

	subtitle_pref_normalize_language(normalized,
	                                 sizeof(normalized),
	                                 language);

	if (normalized[0] == '\0')
		strcpy(normalized, "en");

	strncpy(subtitle_preferred_language,
	        normalized,
	        sizeof(subtitle_preferred_language) - 1);

	subtitle_preferred_language[sizeof(subtitle_preferred_language) - 1] = '\0';

}

const char *subtitle_preferences_get_preferred_language(void)
{
	if (subtitle_preferred_language[0] == '\0')
		return "en";

	return subtitle_preferred_language;
}

int subtitle_preferences_language_matches(const char *track_language,
                                          const char *track_language_ietf,
                                          const char *preferred_language)
{
	const char *preferred;

	preferred = preferred_language;

	if (preferred == 0 || preferred[0] == '\0')
		preferred = subtitle_preferences_get_preferred_language();

	if (track_language_ietf != 0 &&
	    track_language_ietf[0] != '\0' &&
	    !subtitle_pref_code_eq(track_language_ietf, "und") &&
	    subtitle_pref_equivalent(track_language_ietf, preferred))
		return 1;

	if (track_language != 0 &&
	    track_language[0] != '\0' &&
	    !subtitle_pref_code_eq(track_language, "und") &&
	    subtitle_pref_equivalent(track_language, preferred))
		return 1;

	return 0;
}