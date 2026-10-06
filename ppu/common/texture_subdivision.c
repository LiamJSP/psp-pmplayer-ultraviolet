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

#include "texture_subdivision.h"

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
	)
	{
	texture_subdivision_constructor_at(t,
	                                   0,
	                                   0,
	                                   texture_width,
	                                   texture_height,
	                                   texture_step,
	                                   vertex_width,
	                                   vertex_height,
	                                   vertex_x,
	                                   vertex_y);
	}

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
	)
	{
	t->texture_width           = texture_width;
	t->texture_width_remaining = texture_width;
	t->texture_step            = texture_step;
	t->texture_x_plus_texture_width = texture_x + texture_width;

	t->output_texture_y_start = texture_y;
	t->output_texture_x_end   = texture_x;
	t->output_texture_y_end   = texture_y + texture_height;

	t->output_last = 0;

	// texture_step / texture_width = t->vertex_step / vertex_width

	t->vertex_step = vertex_width * (float) texture_step / (float) texture_width;

	t->vertex_x_plus_vertex_width = vertex_x + vertex_width;

	t->output_vertex_y_start = vertex_y;
	t->output_vertex_x_end   = vertex_x;
	t->output_vertex_y_end   = vertex_y + vertex_height;
	}

void texture_subdivision_get(struct texture_subdivision_struct *t)
	{
	t->output_texture_x_start = t->output_texture_x_end;
	t->output_vertex_x_start  = t->output_vertex_x_end;

	if (t->texture_width_remaining > t->texture_step)
		{
		t->texture_width_remaining -= t->texture_step;
		t->output_texture_x_end    += t->texture_step;
		t->output_vertex_x_end     += t->vertex_step;
		}
	else
		{
		t->output_texture_x_end = t->texture_x_plus_texture_width;
		t->output_vertex_x_end  = t->vertex_x_plus_vertex_width;

		t->output_last = 1;
		}
	}
