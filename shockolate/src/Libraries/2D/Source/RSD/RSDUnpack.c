/*

Copyright (C) 2015-2018 Night Dive Studios, LLC.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
 
*/
// Rsd unpacking into a bitmap where row=width.
//
// 68K and PowerPC versions
//

#include "grs.h"
#include <stdbool.h>

//----------------------------------------------------------------------------
// PowerPC version
#define kMinLongLoop 4			// minimum # of bytes to need before using long store loop

#ifdef M9B
#include <string.h>
// M9B: the same format without unaligned word accesses (the original casts
// byte pointers to uint32_t * and reads 16-bit codes at odd addresses; the
// m9b CPU traps on each and emulates it: 97% of the game's misaligned
// accesses and a fifth of its time). Runs and copies go through
// memset/memcpy (words where the alignment allows), the long codes are read
// byte by byte (little-endian, as the data).
uchar *gr_rsd8_unpack(uchar *src, uchar *dest)
{
    for (;;) {
        uint32_t code = *src++, n;
        if (code == 0) {                 // run of bytes: count, value
            n = src[0];
            memset(dest, src[1], n);
            src += 2;
        } else if (code < 0x80) {        // copy
            n = code;
            memcpy(dest, src, n);
            src += n;
        } else if (code > 0x80) {        // skip (zero)
            n = code & 0x7f;
            memset(dest, 0, n);
        } else {                         // long opcode
            uint32_t longcode = src[0] | (uint32_t)src[1] << 8;
            src += 2;
            if (!longcode)
                break;
            if (longcode < 0x8000) {     // skip (zero)
                n = longcode;
                memset(dest, 0, n);
            } else if (longcode < 0xC000) {   // copy
                n = longcode & 0x7fff;
                memcpy(dest, src, n);
                src += n;
            } else {                     // run of bytes
                n = longcode & 0x3fff;
                memset(dest, *src++, n);
            }
        }
        dest += n;
    }
    return dest;
}
#else
uchar *gr_rsd8_unpack(uchar *src, uchar *dest)
 {
 	uchar		code,val;
 	short		count,count2;
 	ushort	longcode;
 	uint32_t		longval, *longdest, *longsrc;
 
 	do
 	 {
 		code = *(src++);
 		if (!code) // run of bytes
 		 {	
 		 	count = *(src++); // get count
 		 	val = *(src++); 	// get val
 		 	
 		 	if (count>=kMinLongLoop)		// if at least kMinLongLoop bytes, do long word stuff
 		 	 {
 		 	 	longval = val + (((uint32_t) val)<<8);
 		 	 	longval += longval<<16;
 		 	 	count2 = count>>2;
 		 	 	count &= 3;
 		 	 	longdest = (uint32_t *) dest;
 		 	 	
 		 	 	while (count2--)
 		 	 	 	*(longdest++) = longval;
 		 	 	dest = (uchar *) longdest;	
 		 	 }
 		 	
 		 	// do rest of bytes
 		 	while (count--)
 		 		*(dest++) = val;
 		 }
 		else if (code<0x80) // dump (copy) bytes
 		 {
 		 	count = code;
 		 	if (code>=kMinLongLoop)		// if at least kMinLongLoop bytes, do long word stuff
 		 	 {
 		 	 	count2 = count>>2;
 		 	 	count &= 3;
 		 	 	longdest = (uint32_t *) dest;
 		 	 	longsrc = (uint32_t *) src;
 		 	 	
 		 	 	while (count2--)
 		 	 	 	*(longdest++) = *(longsrc++);
 		 	 	dest = (uchar *) longdest;	
 		 	 	src = (uchar *) longsrc;	
 		 	 }
 		 	
 		 	// do rest of bytes
 		 	while (count--)
 		 		*(dest++) = *(src++);
 		 }
 		else if (code>0x80) // skip (zero) bytes)
 		 {
 		 	count = code & 0x007f;	// clear high byte
 		 	val = longval = 0L;
 		 	
 		 	if (count>=kMinLongLoop)		// if at least kMinLongLoop bytes, do long word stuff
 		 	 {
 		 	 	count2 = count>>2;
 		 	 	count &= 3;
 		 	 	longdest = (uint32_t *) dest;
 		 	 	
 		 	 	while (count2--)
 		 	 	 	*(longdest++) = longval;
 		 	 	dest = (uchar *) longdest;	
 		 	 }
 		 	
 		 	// do rest of bytes
 		 	while (count--)
 		 		*(dest++) = val;
 		 }
 		else	// long opcode 
 		 {
 		 	longcode = * (ushort *) src; 		 	
 		 	src += 2L;
 		 	
 		 	if (!longcode) break;		// done?
			else if (longcode<0x8000)	// skip (zero)
			 {
	 		 	count = longcode;
	 		 	val = longval = 0L;
	 		 	
	 		 	if (count>=kMinLongLoop)		// if at least kMinLongLoop bytes, do long word stuff
	 		 	 {
	 		 	 	count2 = count>>2;
	 		 	 	count &= 3;
	 		 	 	longdest = (uint32_t *) dest;
	 		 	 	
	 		 	 	while (count2--)
	 		 	 	 	*(longdest++) = longval;
	 		 	 	dest = (uchar *) longdest;	
	 		 	 }
	 		 	
	 		 	// do rest of bytes
	 		 	while (count--)
	 		 		*(dest++) = val;
			 }
			else if (longcode<0xC000)	// dump (copy)
			 {
			 	count = longcode & 0x7fff;	// clear high bit
			 	
	 		 	if (count>=kMinLongLoop)		// if at least kMinLongLoop bytes, do long word stuff
	 		 	 {
	 		 	 	count2 = count>>2;
	 		 	 	count &= 3;
	 		 	 	longdest = (uint32_t *) dest;
	 		 	 	longsrc = (uint32_t *) src;
	 		 	 	
	 		 	 	while (count2--)
	 		 	 	 	*(longdest++) = *(longsrc++);
	 		 	 	dest = (uchar *) longdest;	
	 		 	 	src = (uchar *) longsrc;	
	 		 	 }
	 		 	
	 		 	// do rest of bytes
	 		 	while (count--)
	 		 		*(dest++) = *(src++);
			 }
			else	// run of bytes
			 {
	 		 	count = longcode & 0x3fff;
	 		 	val = *(src++); 	// get val
	 		 	
	 		 	if (count>=kMinLongLoop)		// if at least kMinLongLoop bytes, do long word stuff
	 		 	 {
	 		 	 	longval = val + (((uint32_t) val)<<8);
	 		 	 	longval += longval<<16;
	 		 	 	count2 = count>>2;
	 		 	 	count &= 3;
	 		 	 	longdest = (uint32_t *) dest;
	 		 	 	
	 		 	 	while (count2--)
	 		 	 	 	*(longdest++) = longval;
	 		 	 	dest = (uchar *) longdest;	
	 		 	 }
	 		 	
	 		 	// do rest of bytes
	 		 	while (count--)
	 		 		*(dest++) = val;
			 } 		 	
 		 }
 	 } 
 	while (true);
 	
 	return(dest);
 }
#endif
