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
interface routines
*/

#ifndef movie_interface_h
#define movie_interface_h

#define interface_height 32

extern unsigned char __attribute__((aligned(16))) background_8888[65536];
extern unsigned char __attribute__((aligned(16))) aspect_ratio_8888[6048];
extern unsigned char __attribute__((aligned(16))) numbers_8888[1680];
extern unsigned char __attribute__((aligned(16))) loop_8888[1008];

void draw_interface
	(
	unsigned int scale,
	unsigned int rate,
	unsigned int number_of_frames,
	unsigned int frame_number,
	unsigned int aspect_ratio,
	unsigned int zoom,
	unsigned int luminosity_boost,
	unsigned int audio_stream,
	unsigned int volume_boost,
	unsigned int loop
	);

#endif
