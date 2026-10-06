#include <psptypes.h>
#include "graphics.h"

Color* globalVramBase = (Color*) (0x40000000 | 0x04000000);
Color* globalCachedVramBase = (Color*) (0x04000000);
