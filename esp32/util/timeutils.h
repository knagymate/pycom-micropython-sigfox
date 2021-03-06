/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2015 Daniel Campora
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef __MICROPY_INCLUDED_LIB_TIMEUTILS_H__
#define __MICROPY_INCLUDED_LIB_TIMEUTILS_H__

#include "lib/timeutils/timeutils.h"

typedef int64_t mp_time_t;

void timeutils_seconds_since_epoch_to_struct_time(mp_time_t t,
    timeutils_struct_time_t *tm);

mp_time_t timeutils_seconds_since_epoch(mp_uint_t year, mp_uint_t month,
    mp_uint_t date, mp_uint_t hour, mp_uint_t minute, mp_uint_t second);

mp_time_t timeutils_mktime_since_epoch(mp_uint_t year, mp_int_t month, mp_int_t mday,
    mp_int_t hours, mp_int_t minutes, mp_int_t seconds);

#endif // __MICROPY_INCLUDED_LIB_TIMEUTILS_H__
