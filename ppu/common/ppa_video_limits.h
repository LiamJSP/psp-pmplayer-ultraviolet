#ifndef PPA_VIDEO_LIMITS_H
#define PPA_VIDEO_LIMITS_H

/*
 * Decoder/display contract for this project.  720x480 is the largest coded
 * surface accepted by the PSP AVC path (and by TV-out); the built-in LCD is
 * 480x272.  The project intentionally has no HD/720p or software-resize path.
 */
#define PPA_VIDEO_MAX_CODED_WIDTH   720U
#define PPA_VIDEO_MAX_CODED_HEIGHT  480U
#define PPA_VIDEO_LCD_WIDTH         480U
#define PPA_VIDEO_LCD_HEIGHT        272U

#define PPA_VIDEO_DIMENSIONS_VALID(width, height) \
    ((width) > 0U && (height) > 0U && \
     (width) <= PPA_VIDEO_MAX_CODED_WIDTH && \
     (height) <= PPA_VIDEO_MAX_CODED_HEIGHT)

#endif
