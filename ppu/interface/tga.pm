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


# simple tga lib


package tga;
use strict;
1;


sub write_tga
	{
	my ($tga, $p) = @_;

	((3 * $p->{w} * $p->{h}) == length($p->{d})) or die;

	open(my $f, ">$tga") or die;
	binmode($f) or die;

	print($f pack("C", 0)) or die;
	print($f pack("C", 0)) or die;
	print($f pack("C", 2)) or die;

	print($f pack("C", 0)) or die;
	print($f pack("C", 0)) or die;
	print($f pack("C", 0)) or die;
	print($f pack("C", 0)) or die;
	print($f pack("C", 0)) or die;

	print($f pack("v", 0)) or die;
	print($f pack("v", 0)) or die;
	print($f pack("v", $p->{w})) or die;
	print($f pack("v", $p->{h})) or die;

	print($f pack("C", 24)) or die;
	print($f pack("C", 0)) or die;

	for (0 .. ($p->{h} - 1))
		{
		my $v = substr($p->{d}, -(3 * $p->{w}), 3 * $p->{w}, "");

		print($f $v) or die;
		}

	close($f) or die;
	}


sub init_image
	{
	my ($w, $h, $red, $green, $blue) = @_;

	my $p = {};

	$p->{w} = $w;
	$p->{h} = $h;

	$p->{d} = (chr($blue) . chr($green) . chr($red)) x ($w * $h);

	return($p);
	}


sub setpix
	{
	my ($p, $x, $y, $red, $green, $blue) = @_;

	if ($x >= 0 && $x < $p->{w} && $y >= 0 && $y < $p->{h})
		{
		substr($p->{d}, 3 * $p->{w} * $y + 3 * $x, 3, chr($blue) . chr($green) . chr($red));
		}
	else
		{
		die;
		}
	}


sub getpix
	{
	my ($p, $x, $y) = @_;

	if ($x >= 0 && $x < $p->{w} && $y >= 0 && $y < $p->{h})
		{
		my $d = substr($p->{d}, 3 * $p->{w} * $y + 3 * $x, 3);

		my $v = {};

		$v->{blue}  = ord(substr($d, 0, 1, ""));
		$v->{green} = ord(substr($d, 0, 1, ""));
		$v->{red}   = ord(substr($d, 0, 1, ""));

		return($v);
		}
	else
		{
		die;
		}
	}


sub read_tga
	{
	my ($tga) = @_;


	open(my $f, $tga) or die;
	binmode($f) or die;

	my $d = "";
	while (<$f>)
		{
		$d .= $_;
		}

	close($f) or die;


	my $v;


	$v = substr($d, 0, 1, "");
	my $n = ord($v);


	$v = substr($d, 0, 1, "");
	(ord($v) == 0) or die;
	$v = substr($d, 0, 1, "");
	(ord($v) == 2) or die;


	$v = substr($d, 0, 1, "");
	(ord($v) == 0) or die;
	$v = substr($d, 0, 1, "");
	(ord($v) == 0) or die;
	$v = substr($d, 0, 1, "");
	(ord($v) == 0) or die;
	$v = substr($d, 0, 1, "");
	(ord($v) == 0) or die;
	$v = substr($d, 0, 1, "");
	(ord($v) == 0) or die;


	$v = substr($d, 0, 2, "");
	(unpack("v", $v) == 0) or die;
	$v = substr($d, 0, 2, "");
	(unpack("v", $v) == 0) or die;


	my $p = {};


	$v = substr($d, 0, 2, "");
	$p->{w} = unpack("v", $v);
	$v = substr($d, 0, 2, "");
	$p->{h} = unpack("v", $v);


	$v = substr($d, 0, 1, "");
	(ord($v) == 24) or die;
	$v = substr($d, 0, 1, "");
	(ord($v) == 0) or die;
	
	
	substr($d, 0, $n, "");
	$d = substr($d, 0, 3 * $p->{w} * $p->{h});


	((3 * $p->{w} * $p->{h}) == length($d)) or die;


	$p->{d} = "";
	for (0 .. ($p->{h} - 1))
		{
		$p->{d} .= substr($d, -(3 * $p->{w}), 3 * $p->{w}, "");
		}


	return($p);
	}
