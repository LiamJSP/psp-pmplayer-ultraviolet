#ifndef PPA_PROCESS_H
#define PPA_PROCESS_H
#ifdef __cplusplus
extern "C" {
#endif
/* Callback-safe exit intent. Playback/UI owners perform ordered teardown. */
int ppa_process_exit_requested(void);
#ifdef __cplusplus
}
#endif
#endif
