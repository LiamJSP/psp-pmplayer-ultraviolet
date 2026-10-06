# PMP Mod
# Copyright (C) 2006 jonny
#
# Homepage: http://jonny.leffe.dnsalias.com
# E-mail:   jonny@leffe.dnsalias.com
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.


# generates luminosity textures


use strict;

my $number_of_luminosity_boosts = 16;
my $step                        =  4;

my @luminosity_textures = ();

for (0 .. $number_of_luminosity_boosts - 1)
	{
	my $v = $step * $_;
	
	for (1 .. 4 * 4)
		{
		push @luminosity_textures, $v;
		push @luminosity_textures, $v;
		push @luminosity_textures, $v;
		push @luminosity_textures, 0;
		}
	}

print("static unsigned char __attribute__((aligned(64))) luminosity_textures[64 * number_of_luminosity_boosts] = {" . join(", ", @luminosity_textures) . "};\n");
