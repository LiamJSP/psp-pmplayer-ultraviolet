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

#include<psptypes.h>
#include<string.h>
#include "fgets_mod.h"
#include "libminiconv.h"

#define MAX_FGETS_UTF16_SIZE 1024

char* fgets_utf16_le(char *s, int n, FILE *f) {
	int capacity = n;
	if ( capacity <= 0 || capacity > MAX_FGETS_UTF16_SIZE )
		return NULL;
	unsigned short buf[MAX_FGETS_UTF16_SIZE];
	memset(buf, 0, MAX_FGETS_UTF16_SIZE*sizeof(unsigned short));
	unsigned short* pbuf = buf;
	
	int c = 0;
	while (--n>0) {
		unsigned short tmp = 0;
		c = getc(f);
		if ( c == EOF )
			break;
		tmp = tmp | c;
		c = getc(f);
		if ( c == EOF )
			break;
		tmp = tmp | ( c << 8 );
		*pbuf++ = tmp;
		if ( tmp == 0x000A )
			break;
	}
	if (c == EOF && pbuf == buf)
		return NULL;
	*pbuf++ = 0x0000;
	char* temp_str = miniConvUTF16LEConv(buf);
	if (temp_str != NULL) {
		strncpy(s, temp_str, (size_t)capacity - 1U);
		s[capacity - 1] = '\0';
	}
	else
		s[0] = '\0';
	return s;
}

char* fgets_utf16_be(char *s, int n, FILE *f) {
	int capacity = n;
	if ( capacity <= 0 || capacity > MAX_FGETS_UTF16_SIZE )
		return NULL;
	unsigned short buf[MAX_FGETS_UTF16_SIZE];
	memset(buf, 0, MAX_FGETS_UTF16_SIZE*sizeof(unsigned short));
	unsigned short* pbuf = buf;
	
	int c = 0;
	while (--n>0) {
		unsigned short tmp = 0;
		c = getc(f);
		if ( c == EOF )
			break;
		tmp = tmp | c;
		c = getc(f);
		if ( c == EOF )
			break;
		tmp = tmp | ( c << 8 );
		*pbuf++ = tmp;
		if ( tmp == 0x0A00 )
			break;
	}
	if (c == EOF && pbuf == buf)
		return NULL;
	*pbuf++ = 0x0000;
	char* temp_str = miniConvUTF16BEConv(buf);
	if (temp_str != NULL) {
		strncpy(s, temp_str, (size_t)capacity - 1U);
		s[capacity - 1] = '\0';
	}
	else
		s[0] = '\0';
	return s;
}
