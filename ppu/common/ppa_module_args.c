#include "ppa_module_args.h"

#include <string.h>

int ppa_module_build_args(char *buffer, size_t capacity,
                          const char *execfile, int argc,
                          char *const argv[])
{
    size_t used = 0U;
    int i;

    if (buffer == 0 || capacity == 0U || execfile == 0 || argc < 0 ||
        (argc != 0 && argv == 0))
        return -1;

    for (i = -1; i < argc; ++i) {
        const char *value = i < 0 ? execfile : argv[i];
        size_t length;
        if (value == 0)
            return -1;
        length = strlen(value) + 1U;
        if (length > capacity - used)
            return -1;
        memcpy(buffer + used, value, length);
        used += length;
    }
    return used <= 0x7fffffffU ? (int)used : -1;
}
