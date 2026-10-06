#ifndef subtitle_preferences_h__
#define subtitle_preferences_h__

#ifdef __cplusplus
extern "C" {
#endif

void subtitle_preferences_set_preferred_language(const char *language);
const char *subtitle_preferences_get_preferred_language(void);

int subtitle_preferences_language_matches(const char *track_language,
                                          const char *track_language_ietf,
                                          const char *preferred_language);

#ifdef __cplusplus
}
#endif

#endif