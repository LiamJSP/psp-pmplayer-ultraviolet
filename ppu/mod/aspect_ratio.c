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

#include "aspect_ratio.h"
#include "common/m33sdk.h"

struct aspect_ratio_struct aspect_ratios[number_of_aspect_ratios] =
	{
	{"Original",   0,   0, 0, 0},
	{"4 / 3",      4,   3, 0, 0},
	{"16 / 9",   480, 272, 0, 0},
	{"2.35",     235, 100, 0, 0}
	};

void aspect_ratio_struct_init_psplcd(unsigned int width, unsigned int height, unsigned int display_width, unsigned int display_height)
	{
	aspect_ratios[0].width  = width;
	aspect_ratios[0].height = height;
	if ( display_width > 480 || display_height > 272 ) 
		{
		if (272 * display_width > 480 * display_height) 
			{
			aspect_ratios[0].psp_width  = 480;
			aspect_ratios[0].psp_height = aspect_ratios[0].psp_width * display_height / display_width;
			}
		else
			{
			aspect_ratios[0].psp_height = 272;
			aspect_ratios[0].psp_width  = aspect_ratios[0].psp_height * display_width / display_height;
			}
		}
	else 
		{
		aspect_ratios[0].psp_width  = display_width;
		aspect_ratios[0].psp_height = display_height;
		}

	int i = 1;
	for (; i < number_of_aspect_ratios; i++)
		{
		// width / height > 480 / 272

		if (272 * aspect_ratios[i].width > 480 * aspect_ratios[i].height)
			{
			// height / width = psp_height / psp_width

			aspect_ratios[i].psp_width  = 480;
			aspect_ratios[i].psp_height = aspect_ratios[i].psp_width * aspect_ratios[i].height / aspect_ratios[i].width;
			}
		else
			{
			// width / height = psp_width / psp_height

			aspect_ratios[i].psp_height = 272;
			aspect_ratios[i].psp_width  = aspect_ratios[i].psp_height * aspect_ratios[i].width / aspect_ratios[i].height;
			}
		}
	}

void aspect_ratio_struct_init_tvout(unsigned int width, unsigned int height, unsigned int display_width, unsigned int display_height, int tv_aspectratio, int tv_width, int tv_height)
	{
	aspect_ratios[0].width  = display_width;
	aspect_ratios[0].height = display_height;

	int i = 0;
	if ( tv_aspectratio == 0 ) // tv ar is 16:9
		{
		for (; i < number_of_aspect_ratios; i++)
			{
			if (272 * aspect_ratios[i].width > 480 * aspect_ratios[i].height)
				{
				// height / width = psp_height / psp_width
	
				aspect_ratios[i].psp_width  = tv_width;
				aspect_ratios[i].psp_height = 480 * aspect_ratios[i].height / aspect_ratios[i].width * tv_height / 272;
				}
			else
				{
				// width / height = psp_width / psp_height
	
				aspect_ratios[i].psp_height = tv_height;
				aspect_ratios[i].psp_width  = 272 * aspect_ratios[i].width / aspect_ratios[i].height * tv_width / 480;
				}
			}
		}
	else // tv ar is 4:3
		{
		for (; i < number_of_aspect_ratios; i++)
			{
			aspect_ratios[i].psp_width = tv_width;
			aspect_ratios[i].psp_height = (tv_height * aspect_ratios[i].height * 4) / (aspect_ratios[i].width * 3);
			}
		}
	aspect_ratios[0].width  = width;
	aspect_ratios[0].height = height;
	}

void aspect_ratio_struct_init(unsigned int width, unsigned int height, unsigned int display_width, unsigned int display_height, int psp_type, int tv_aspectratio, int tv_width, int tv_height, int video_mode)
	{
	if( !m33IsTVOutSupported(psp_type) )
		aspect_ratio_struct_init_psplcd(width, height, display_width, display_height);
	else
		{
		if ( video_mode == 0 )
			aspect_ratio_struct_init_psplcd(width, height, display_width, display_height);
		else
			aspect_ratio_struct_init_tvout(width, height, display_width, display_height, tv_aspectratio, tv_width, tv_height);
		}
	}
