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
texture subdivision routines
*/

#ifndef texture_subdivision_h
#define texture_subdivision_h

struct texture_subdivision_struct
	{
	short texture_width;
	short texture_width_remaining;
	short texture_step;
	short texture_x_plus_texture_width;

	float vertex_step;
	float vertex_x_plus_vertex_width;

	short output_texture_x_start;
	short output_texture_y_start;
	short output_texture_x_end;
	short output_texture_y_end;

	float output_vertex_x_start;
	float output_vertex_y_start;
	float output_vertex_x_end;
	float output_vertex_y_end;

	int output_last;
	};

void texture_subdivision_constructor
	(
	struct texture_subdivision_struct *t,
	short texture_width,
	short texture_height,
	short texture_step,
	float vertex_width,
	float vertex_height,
	float vertex_x,
	float vertex_y
	);

/* Source-origin variant used by UI image blits. Most video paths bind a
 * texture pointer at the desired first texel and should keep using the legacy
 * zero-origin constructor above. */
void texture_subdivision_constructor_at
	(
	struct texture_subdivision_struct *t,
	short texture_x,
	short texture_y,
	short texture_width,
	short texture_height,
	short texture_step,
	float vertex_width,
	float vertex_height,
	float vertex_x,
	float vertex_y
	);

void texture_subdivision_get(struct texture_subdivision_struct *t);

#endif
