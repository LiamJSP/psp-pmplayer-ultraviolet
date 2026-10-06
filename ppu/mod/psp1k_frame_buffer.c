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

static void* frame_buffer0 = 0;
static void* frame_buffer1 = 0;
static void* frame_buffer2 = 0;
static void* frame_buffer3 = 0;

int psp1k_init_frame_buffer() {
	if ( frame_buffer0 == 0 ) {
		frame_buffer0 = malloc_64(557056);
	}
	if ( !frame_buffer0 )
		return 0;
	if ( frame_buffer1 == 0 ) {
		frame_buffer1 = malloc_64(557056);
	}
	if ( !frame_buffer1 )
		return 0;
	if ( frame_buffer2 == 0 ) {
		frame_buffer2 = malloc_64(557056);
	}
	if ( !frame_buffer2 )
		return 0;
	if ( frame_buffer3 == 0 ) {
		frame_buffer3 = malloc_64(557056);
	}
	if ( !frame_buffer3 )
		return 0;
	return 1;
}

void* psp1k_get_frame_buffer(int frame) {
	switch(frame) {
		case 0 : return frame_buffer0;
			break;
		case 1 : return frame_buffer1;
			break;
		case 2 : return frame_buffer2;
			break;
		case 3 : return frame_buffer3;
			break;
		default: return 0;
			break; 
	}
}