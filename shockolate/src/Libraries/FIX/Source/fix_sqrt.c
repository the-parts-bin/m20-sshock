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
//=================================================================
//
//		System Shock - ©1994-1995 Looking Glass Technologies, Inc.
//
//		FIX_SQRT.c	-	Square root routine for fixed-point
// numbers.  Adapted from 80386 asm.
//
//=================================================================

//--------------------
//  Includes
//--------------------
#include "fix.h"
#include "lg.h"
#include <math.h>

//-----------------------------------------------------------------
//  Calculate the square root of a fixed-point number.
//-----------------------------------------------------------------
#ifdef M9B
// M9B: square roots in integers (no FPU: sqrtf, sqrt and sqrtl are slow
// library code here), rounded down as the float versions' casts did.
uint32_t isqrt64(uint64_t n) {
    uint64_t res = 0, bit = (uint64_t)1 << 62;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else
            res >>= 1;
        bit >>= 2;
    }
    return (uint32_t)res;
}

static uint32_t isqrt32(uint32_t n) {
    uint32_t res = 0, bit = 1u << 30;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else
            res >>= 1;
        bit >>= 2;
    }
    return res;
}

fix fix_sqrt(fix num) {
    return num > 0 ? (fix)isqrt64((uint64_t)num << 16) : 0;
}

int long_sqrt(int num) {
    if (num < 0) {
        ERROR("long_sqrt of negative number!");
        return (0);
    }
    return (int)isqrt32((uint32_t)num);
}
#else
fix fix_sqrt(fix num) {
    // fix	res = long_sqrt(num);

    float f = fix_float(num);
    f = sqrtf(f);

    // Make the number a fix and return it
    return fix_from_float(f);
}

//-----------------------------------------------------------------
//  Calculate the square root of a long number.
//-----------------------------------------------------------------
int long_sqrt(int num) {
    // WH dunno, needed?
    if (num == 0)
        return (0);
    // A bit of error checking.
    if (num < 0) {
        ERROR("long_sqrt of negative number!");
        return (0);
    }

    return (int32_t)sqrt(num);
}
#endif
