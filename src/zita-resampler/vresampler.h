// ----------------------------------------------------------------------------
//
//  Copyright (C) 2006-2023 Fons Adriaensen <fons@linuxaudio.org>
//  Copyright (C) 2023 falkTX <falktx@falktx.com>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------


#ifndef __VRESAMPLER_H
#define __VRESAMPLER_H


#include "resampler-table.h"


class VResampler
{
public:

    VResampler (void) noexcept;
    ~VResampler (void);

    bool setup (double       ratio,
                unsigned int nchan,
                unsigned int hlen);

    bool setup (double       ratio,
                unsigned int nchan,
                unsigned int hlen,
                double       frel);

    void   clear (void);
    bool   reset (void) noexcept;
    int    nchan (void) const noexcept { return _nchan; }
    int    inpsize (void) const noexcept;
    double inpdist (void) const noexcept;
    bool   process (void);

    void set_phase (double p);
    void set_rrfilt (double t);
    void set_rratio (double r);

    unsigned int         inp_count;
    unsigned int         out_count;
    const float *const  *inp_data;
    float*              *out_data;

private:

    enum { NPHASE = 120 };

    Resampler_table     *_table;
    unsigned int         _nchan;
    unsigned int         _inmax;
    unsigned int         _index;
    unsigned int         _nread;
    unsigned int         _nzero;
    double               _ratio;
    double               _phase;
    double               _pstep;
    double               _qstep;
    double               _wstep;
    float               *_buff;
    float               *_c1;
    float               *_c2;
    void                *_dummy [8];
};


#endif
