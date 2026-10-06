#ifndef PPA_VIDEO_OUTPUT_H
#define PPA_VIDEO_OUTPUT_H

/* Both browser and playback consume saved overscan. Reject negative or huge
 * margins before unsigned viewport arithmetic, retaining a nonempty canvas. */
static inline int ppa_video_output_margin(int value, int maximum)
{
    return value < 0 ? 0 : (value > maximum ? maximum : value);
}

#endif
