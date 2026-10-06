#ifndef PPA_SUBTITLE_ASS_H
#define PPA_SUBTITLE_ASS_H

#include <stdio.h>
#include "subtitle_parse.h"

/* ASS and SSA share the same Dialogue timing/text fields used by PPA. */
struct subtitle_frame_struct *subtitle_parse_ass(FILE *file,
                                                 char *charset,
                                                 unsigned int rate,
                                                 unsigned int scale);

#endif
