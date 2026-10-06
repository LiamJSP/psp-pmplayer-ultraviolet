/* 
 *	Copyright (C) 2006 cooleyes
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
 
#include "base64.h"

static char *base64_encoding = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char get_base64_value(char ch) {
	if ((ch >= 'A') && (ch <= 'Z')) 
		return ch - 'A'; 
	if ((ch >= 'a') && (ch <= 'z')) 
		return ch - 'a' + 26; 
	if ((ch >= '0') && (ch <= '9')) 
		return ch - '0' + 52; 
	switch (ch) { 
		case '+': 
			return 62; 
		case '/': 
			return 63; 
		case '=': /* base64 padding */ 
			return 0; 
		default: 
			return 0; 
	} 
}

int base64encode(char * buffer, const unsigned char * src, int len ) { 
	int buflen = 0; 

	while(len>0){
		*buffer++ = base64_encoding[ (src[0] >> 2 ) & 0x3f];
		if(len>2){
			*buffer++ = base64_encoding[((src[0] & 3) << 4) | (src[1] >> 4)];
			*buffer++ = base64_encoding[((src[1] & 0xF) << 2) | (src[2] >> 6)];
			*buffer++ = base64_encoding[src[2] & 0x3F];
		}
		else{
			switch(len){
				case 1:
					*buffer++ = base64_encoding[(src[0] & 3) << 4 ];
					*buffer++ = '=';
					*buffer++ = '=';
					break;
				case 2: 
					*buffer++ = base64_encoding[((src[0] & 3) << 4) | (src[1] >> 4)]; 
					*buffer++ = base64_encoding[(src[1] & 0x0F) << 2];
					*buffer++ = '='; 
					break; 
			} 
		} 

		src +=3; 
		len -=3; 
		buflen +=4; 
	} 

	*buffer = 0; 
	return buflen; 
} 

int base64decode( unsigned char * buffer, const char * src, int len ){
	int i;
	int padding = 0;
	if (buffer == 0 || src == 0 || len < 0 || len % 4)
		return -1;
	if (len == 0)
		return 0;
	if (src[len - 1] == '=') ++padding;
	if (src[len - 2] == '=') ++padding;
	/* Validate before writing anything. The caller supplies capacity for
	 * (len / 4) * 3 - padding bytes and owns any string terminator. */
	for (i = 0; i < len - padding; ++i) {
		unsigned char c = (unsigned char)src[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '+' || c == '/'))
			return -1;
	}
	unsigned char chunk[4];
	int parsenum=0;

	while(len>0){
		chunk[0] = get_base64_value(src[0]); 
		chunk[1] = get_base64_value(src[1]); 
		chunk[2] = get_base64_value(src[2]); 
		chunk[3] = get_base64_value(src[3]); 

		*buffer++ = (chunk[0] << 2) | (chunk[1] >> 4); 
		++parsenum;
		if (len != 4 || padding < 2) {
			*buffer++ = (chunk[1] << 4) | (chunk[2] >> 2);
			++parsenum;
		}
		if (len != 4 || padding == 0) {
			*buffer++ = (chunk[2] << 6) | chunk[3];
			++parsenum;
		}

		src+=4;
		len-=4;
	}

	return parsenum;
} 
