/* 
 *	Copyright (C) 2009 cooleyes
 *	eyes.cooleyes@gmail.com 
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "psp1k_frame_buffer.h"

/* 4 x 512 x 272 x RGBA8888; allocated in main RAM, not GE eDRAM. */
static void *frame_buffers[4];

int psp1k_init_frame_buffer(void)
{
    int i;
    for (i = 0; i < 4; ++i) {
        if (!frame_buffers[i]) frame_buffers[i] = malloc_64(557056);
        if (!frame_buffers[i]) {
            /* Called only at startup before any surface is device-visible. */
            int j;
            for (j = 0; j < 4; ++j) {
                free_64(frame_buffers[j]);
                frame_buffers[j] = 0;
            }
            return 0;
        }
    }
    return 1;
}

void *psp1k_get_frame_buffer(int frame)
{
    return frame >= 0 && frame < 4 ? frame_buffers[frame] : 0;
}
