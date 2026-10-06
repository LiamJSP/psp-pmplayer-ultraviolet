#include "subtitle_ass.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "common/fgets_mod.h"
#include "common/libminiconv.h"
#include "common/mem64.h"

static fgets_function subtitle_ass_reader_for_charset(const char *charset)
{
    if (charset != 0 && strcasecmp(charset, "UTF-16LE") == 0)
        return fgets_utf16_le;
    if (charset != 0 && strcasecmp(charset, "UTF-16BE") == 0)
        return fgets_utf16_be;
    return fgets;
}

static void subtitle_ass_convert_text(struct subtitle_frame_struct *frame,
                                      const char *charset)
{
    char *converted = 0;

    if (miniConvHaveSubtitleConv(charset))
        converted = miniConvSubtitleConv((const unsigned char *)frame->p_string, charset);
    else if (charset != 0 && strcasecmp(charset, "DEFAULT") == 0 &&
             miniConvHaveDefaultSubtitleConv())
        converted = miniConvDefaultSubtitleConv((const unsigned char *)frame->p_string);

    if (converted != 0) {
        memset(frame->p_string, 0, sizeof(frame->p_string));
        strncpy(frame->p_string, converted, sizeof(frame->p_string) - 1U);
    }
}

struct subtitle_frame_struct *subtitle_parse_ass(FILE *file,
                                                 char *charset,
                                                 unsigned int rate,
                                                 unsigned int scale)
{
    struct subtitle_frame_struct *frame;
    fgets_function read_line;
    char line[1024];
    char *text;
    int a1, a2, a3, a4;
    int b1, b2, b3, b4;
    int i;
    int out;

    if (file == 0 || rate == 0 || scale == 0)
        return 0;

    frame = (struct subtitle_frame_struct *)malloc_64(sizeof(*frame));
    if (frame == 0)
        return 0;
    subtitle_frame_safe_constructor(frame);

    read_line = subtitle_ass_reader_for_charset(charset);
    for (;;) {
        memset(line, 0, sizeof(line));
        if (!read_line(line, sizeof(line), file)) {
            free_64(frame);
            return 0;
        }

        /* Both ASS (numeric layer) and SSA (name/mark) place an arbitrary
         * first field before the first comma. */
        if (sscanf(line,
                   "Dialogue: %*[^,],%9d:%2d:%2d%*[.:]%2d,%9d:%2d:%2d%*[.:]%2d",
                   &a1, &a2, &a3, &a4,
                   &b1, &b2, &b3, &b4) == 8 &&
            a4 >= 0 && b4 >= 0 &&
            subtitle_time_to_frame(a1, a2, a3, a4 * 10, rate, scale,
                                   &frame->p_start_frame) &&
            subtitle_time_to_frame(b1, b2, b3, b4 * 10, rate, scale,
                                   &frame->p_end_frame) &&
            frame->p_end_frame >= frame->p_start_frame)
            break;
    }

    text = line;
    for (i = 0; i < 9; ++i) {
        text = strchr(text, ',');
        if (text == 0)
            break;
        ++text;
    }
    if (text == 0) {
        free_64(frame);
        return 0;
    }

    frame->p_num_lines = 0;
    out = 0;
    for (i = 0; text[i] != 0 && out < max_subtitle_string - 1; ++i) {
        char c = text[i];

        if (c == '{') {
            while (text[i] != 0 && text[i] != '}')
                ++i;
            if (text[i] == 0)
                break;
            continue;
        }

        if (c == '\\' && text[i + 1] != 0) {
            c = text[++i];
            if (c == 'N' || c == 'n') {
                frame->p_string[out++] = '\n';
                ++frame->p_num_lines;
            }
            continue;
        }

        if (c == '\r' || c == '\n')
            continue;

        if (frame->p_num_lines == 0)
            frame->p_num_lines = 1;
        frame->p_string[out++] = c;
    }
    frame->p_string[out] = 0;

    subtitle_ass_convert_text(frame, charset);
    return frame;
}
