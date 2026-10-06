/*
subtitle parsing layer
*/

#ifndef subtitle_parse_h__
#define subtitle_parse_h__

#include <stdio.h>
#include "movie_file.h"

#define max_subtitle_string 1024

/* One shared, overflow-checked conversion for external subtitle timestamps. */
int subtitle_time_to_frame(int hour, int minute, int second, int millisecond,
                          unsigned int rate, unsigned int scale,
                          unsigned int *frame);

struct subtitle_frame_struct {
	unsigned int		p_start_frame;
	unsigned int		p_end_frame;
	unsigned int		p_num_lines;
	char			p_string[max_subtitle_string];

	struct subtitle_frame_struct *next;
	struct subtitle_frame_struct *prev;
};

struct subtitle_parse_struct {
	char				filename[1024];
	struct subtitle_frame_struct	*p_sub_frame;
	unsigned int			p_num_sub_frames;
	struct subtitle_frame_struct	*p_cur_sub_frame;
	FILE				*p_in;
};

#define MAX_SUBTITLES 48
extern struct subtitle_parse_struct subtitle_parser[MAX_SUBTITLES];

void subtitle_frame_safe_constructor(struct subtitle_frame_struct *p);
void subtitle_frame_safe_destructor(struct subtitle_frame_struct *p);
void subtitle_parse_safe_constructor(struct subtitle_parse_struct *p);
char *subtitle_parse_search(struct movie_file_struct* movie, unsigned int rate, unsigned int scale, unsigned int *num_subtitles);
char *subtitle_parse_open(struct subtitle_parse_struct *p, char *s, char* charset, unsigned int rate, unsigned int scale);
struct subtitle_frame_struct* subtitle_parse_add_frame(struct subtitle_parse_struct *p, struct subtitle_frame_struct *cur, struct subtitle_frame_struct *f);
void subtitle_parse_close(struct subtitle_parse_struct *p);
char* subtitle_parse_get_frame(struct subtitle_parse_struct *p, struct subtitle_frame_struct **f, unsigned int frame);

int strconv(char* s, const char a, const char b);

#endif
