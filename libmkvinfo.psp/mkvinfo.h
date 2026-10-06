#ifndef __MKVINFO_H__
#define __MKVINFO_H__

#include "mkvinfo_type.h"

#ifdef __cplusplus
extern "C" {
#endif

mkvinfo_t* mkvinfo_open(const char* filename);
mkvinfo_t* mkvinfo_open_metadata(const char* filename);
void mkvinfo_close(mkvinfo_t* info);


#ifdef __cplusplus
}
#endif

#endif