/*
PMP Mod
Copyright (C) 2006 jonny

Homepage: http://jonny.leffe.dnsalias.com
E-mail:   jonny@leffe.dnsalias.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
aspect ratio calculations
*/

#ifndef aspect_ratio_h
#define aspect_ratio_h

#define number_of_aspect_ratios 4

struct aspect_ratio_struct
	{
	char *description;

	unsigned int width;
	unsigned int height;
	unsigned int psp_width;
	unsigned int psp_height;
	};

extern struct aspect_ratio_struct aspect_ratios[number_of_aspect_ratios];

void aspect_ratio_struct_init(unsigned int width, unsigned int height, unsigned int display_width, unsigned int display_height, int psp_type, int tv_aspectratio, int tv_width, int tv_height, int video_mode);

#endif
