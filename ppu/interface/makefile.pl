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


# generates graphics data


use strict;
use tga;


sub out
	{
	my ($tga, $txt) = @_;


	my @v = ();
	my $p = tga::read_tga($tga);


	for (0 .. $p->{h} - 1)
		{
		my $y = $_;

		for (0 .. $p->{w} - 1)
			{
			my $x = $_;

			my $v = tga::getpix($p, $x, $y);

			push(@v, $v->{red});
			push(@v, $v->{green});
			push(@v, $v->{blue});
			push(@v, 255);
			}
		}


    	print "unsigned char __attribute__((aligned(16))) $txt" . "[" . (4 * $p->{w} * $p->{h}) . "] = ";
	print "{";
	print(join(", ", @v));
	print "};\n";
	}

sub out_ext
	{
	my ($tga, $txt) = @_;


	my @v = ();
	my $p = tga::read_tga($tga);


	for (0 .. $p->{h} - 1)
		{
		my $y = $_;

		for (0 .. $p->{w} - 1)
			{
			my $x = $_;

			my $v = tga::getpix($p, $x, $y);
			if ($y > 23)
				{
				push(@v, 0);
				push(@v, 0);
				push(@v, 0);
				push(@v, 0);
				}
			else
				{
				push(@v, $v->{red});
				push(@v, $v->{green});
				push(@v, $v->{blue});
				push(@v, 255);
				}
			}
		}


    	print "unsigned char __attribute__((aligned(16))) $txt" . "[" . (4 * $p->{w} * $p->{h}) . "] = ";
	print "{";
	print(join(", ", @v));
	print "};\n";
	}

out_ext("background.tga",   "background_8888");
out("aspect_ratio.tga", "aspect_ratio_8888");
out("numbers.tga",      "numbers_8888");
out("loop.tga",         "loop_8888");
