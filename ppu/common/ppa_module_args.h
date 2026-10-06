#ifndef PPA_MODULE_ARGS_H
#define PPA_MODULE_ARGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the PSP module-start argument block as consecutive NUL-terminated
 * strings. Returns the byte count or -1 if the inputs cannot fit safely. */
int ppa_module_build_args(char *buffer, size_t capacity,
                          const char *execfile, int argc,
                          char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif
