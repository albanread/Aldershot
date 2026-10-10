/* Copyright (c) 2021 RISC OS Open Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is a translation into C of RISC OS Open's VFPSupport source
 * (Sources/HWSupport/VFPSupport: s.Trig64, s.ArcTrig64, s.Power64, s.CMath).
 * Those are in turn translations to AArch32 of the following, whose
 * notices ROOL's source carries and which are kept here.
 *
 *   aocl-libm-ose/src/optmized/sin.c and tan.c
 *       Copyright (C) 2008-2020 Advanced Micro Devices, Inc. All rights
 *       reserved.  SPDX-License-Identifier: BSD-3-Clause
 *   aocl-libm-ose/src/ref/remainder_piby2.c
 *       Copyright (c) 2002-2019 Advanced Micro Devices, Inc.
 *       SPDX-License-Identifier: MIT
 *   openlibm/src/e_asin.c, s_atan.c, e_atan2.c, e_log.c, e_log10.c, e_exp.c
 *       and e_pow.c
 *       Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *       Developed at SunPro, a Sun Microsystems, Inc. business.
 *       (some also Copyright (C) 2004 by Sun Microsystems, Inc.,
 *       developed at SunSoft, a Sun Microsystems, Inc. business.)
 *       Permission to use, copy, modify, and distribute this software is
 *       freely granted, provided that this notice is preserved.
 *
 * The ldexp function follows s.CMath, which is
 *
 * Copyright 2021 RISC OS Open Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Mixed provenance.  Different parts of this file come under different
 * licences: ROOL's BSD notice above for the trigonometric, inverse
 * trigonometric and power code, ROOL's Apache licence for ldexp, and the
 * AMD and Sun notices for the algorithms beneath them.  The notices are kept
 * as ROOL's source carries them.  The full AMD BSD-3-Clause text is not
 * reproduced, because ROOL's source does not carry it.
 */

/* elementary.h: the constants of VFPSupport's elementary functions.
 * They are taken from its sources (HWSupport/VFPSupport s/Trig64,
 * s/ArcTrig64, s/Power64, s/CMath) and keep the labels used there. Each
 * DCD pair (low word, high word) is written as the bits of one double.
 * Each name is prefixed with its source file (trig_, arc_, pow_, cm_).
 * This header is for elementary.c only.
 */
#ifndef ROSGD_VFPSUPPORT_ELEMENTARY_H
#define ROSGD_VFPSUPPORT_ELEMENTARY_H

#include <stdint.h>

/* K(bits): a double from its bits (elementary.c) */

#define trig_dpiby2 K(0x3FF921FB54442D18u)
#define trig_dpiby4 K(0x3FE921FB54442D18u)
#define trig_dpiby4r K(0x3C81A62633145C06u)
#define trig_done K(0x3FF0000000000000u)
#define trig_dhalf K(0x3FE0000000000000u)
#define trig_dminussixth K(0xBFC5555555555555u)
static const uint64_t trig_dCsin[6] = {
    0xBFC5555555555555u, 0x3F81111111110BB3u, 0xBF2A01A019E83E5Cu, 0x3EC71DE3796CDE01u,
    0xBE5AE600B42FDFA7u, 0x3DE5E0B2F9A43BB8u,
};
static const uint64_t trig_dCcos[6] = {
    0x3FA5555555555555u, 0xBF56C16C16C16967u, 0x3EFA01A019F4EC91u, 0xBE927E4FA17F667Bu,
    0x3E21EEB690382EECu, 0xBDA907DB47258AA7u,
};
#define trig_dthird K(0x3FD5555555555555u)
#define trig_d5e5 K(0x411E848000000000u)
#define trig_d0p68 K(0x3FE5C28F5C28F5C3u)
static const uint64_t trig_dCtan[7] = {
    0x3FD7D50F6638564Au, 0xBF977C24C7569ABBu, 0x3F2D5DAF289C385Au, 0x3FF1DFCB8CAA40B8u,
    0xBFE08046499EB90Fu, 0x3F9AB0F4F80A0ACFu, 0xBF2E7517EF6D98F8u,
};
#define trig_d2uponpi K(0x3FE45F306DC9C883u)
#define trig_dpi1 K(0x3FF921FB54400000u)
#define trig_dpi2 K(0x3DD0B4611A626331u)
#define trig_dpi3 K(0x3DD0B4611A600000u)
#define trig_dpi4 K(0x3BA3198A2E037073u)
#define trig_dpi5 K(0x3DD0B4612E000000u)
#define trig_dpi6 K(0x397B839A252049C1u)
#define trig_d5e6 K(0x415312D000000000u)
#define trig_d18p52 K(0x4338000000000000u)
#define trig_piby2_lead K(0x3FF921FB54442D18u)
#define trig_piby2_part1 K(0x3FF921FB50000000u)
#define trig_piby2_part2 K(0x3E5110B460000000u)
#define trig_piby2_part3 K(0x3C91A62633145C06u)
#define arc_done K(0x3FF0000000000000u)
#define arc_dhalf K(0x3FE0000000000000u)
#define arc_dqnan K(0x7FF8000000000000u)
static const uint64_t arc_dPSin[6] = {
    0x3FC5555555555555u, 0xBFD4D61203EB6F7Du, 0x3FC9C1550E884455u, 0xBFA48228B5688F3Bu,
    0x3F49EFE07501B288u, 0x3F023DE10DFDF709u,
};
static const uint64_t arc_dQSin[4] = {
    0xC0033A271C8A2D4Bu, 0x40002AE59C598AC8u, 0xBFE6066C1B8D0159u, 0x3FB3B8C5B12E9282u,
};
#define arc_dpi K(0x400921FB54442D18u)
#define arc_dpir K(0x3CA1A62633145C07u)
#define arc_dpiby4 K(0x3FE921FB54442D18u)
#define arc_d1point5 K(0x3FF8000000000000u)
static const uint64_t arc_dtanid[8] = {
    0x3FDDAC670561BB4Fu, 0x3C7A2B7F222F65E2u, 0x3FE921FB54442D18u, 0x3C81A62633145C07u,
    0x3FEF730BD281F69Bu, 0x3C7007887AF0CBBDu, 0x3FF921FB54442D18u, 0x3C91A62633145C07u,
};
#define arc_dpiby2 K(0x3FF921FB54442D18u)
#define arc_dpiby2r K(0x3C91A62633145C07u)
static const uint64_t arc_dCatanE[6] = {
    0x3FD555555555550Du, 0x3FC24924920083FFu, 0x3FB745CDC54C206Eu, 0x3FB10D66A0D03D51u,
    0x3FA97B4B24760DEBu, 0x3F90AD3AE322DA11u,
};
static const uint64_t arc_dCatanO[5] = {
    0xBFC999999998EBC4u, 0xBFBC71C6FE231671u, 0xBFB3B0F2AF749A6Du, 0xBFADDE2D52DEFD9Au,
    0xBFA2B4442C6A6C2Fu,
};
#define pow_dhalf K(0x3FE0000000000000u)
#define pow_dtwo54 K(0x4350000000000000u)
#define pow_dln2_20 K(0x3FE62E42FEE00000u)
#define pow_dln2_20r K(0x3DEA39EF35793C76u)
#define pow_dlog2 K(0x3FD34413509F6000u)
#define pow_dlog2r K(0x3D59FEF311F12B36u)
#define pow_d1uponln10 K(0x3FDBCB7B15200000u)
#define pow_d1uponln10r K(0x3DBB9438CA9AADD5u)
#define pow_done K(0x3FF0000000000000u)
#define pow_dexpmax K(0x40862E42FEFA39EFu)
#define pow_dexpmin K(0xC0874910D52D3051u)
#define pow_de1 K(0x4005BF0A8B145769u)
static const uint64_t pow_dCexp[5] = {
    0x3FC555555555553Eu, 0xBF66C16C16BEBD93u, 0x3F11566AAF25DE2Cu, 0xBEBBBD41C5D26BF1u,
    0x3E66376972BEA4D0u,
};
#define pow_d2pow1023 K(0x7FE0000000000000u)
#define pow_d1uponln2 K(0x3FF71547652B82FEu)
static const uint64_t pow_dClogO[4] = {
    0x3FE5555555555593u, 0x3FD2492494229359u, 0x3FC7466496CB03DEu, 0x3FC2F112DF3E5244u,
};
static const uint64_t pow_dClogE[3] = {
    0x3FD999999997FA04u, 0x3FCC71C51D8E78AFu, 0x3FC39A09D078C69Fu,
};
static const uint64_t pow_dCtaylor[3] = {
    0x3FE0000000000000u, 0x3FD5555555555555u, 0x3FD0000000000000u,
};
#define pow_d1uponln2s K(0x3FF7154760000000u)
#define pow_d1uponln2sr K(0x3E54AE0BF85DDF44u)
#define pow_d2pow53 K(0x4340000000000000u)
#define pow_dhuge K(0x7E37E43C8800759Cu)
#define pow_dtiny K(0x01A56E1FC2F8F359u)
#define pow_d3over2 K(0x3FF8000000000000u)
static const uint64_t pow_dClogx[6] = {
    0x3FE3333333333303u, 0x3FDB6DB6DB6FABFFu, 0x3FD55555518F264Du, 0x3FD17460A91D4101u,
    0x3FCD864A93C9DB65u, 0x3FCA7E284A454EEFu,
};
#define pow_dunity K(0x3FF0000000000000u)
#define pow_d2o3ln2 K(0x3FEEC709DC3A03FDu)
#define pow_d2o3ln2_28 K(0x3FEEC709E0000000u)
#define pow_d2o3ln2_28r K(0xBE3E2FE0145B01F5u)
#define pow_dovftie K(0x3C971547652B82FEu)
#define pow_dCdp K(0x3FE2B80340000000u)
#define pow_dCdpr K(0x3E4CFDEB43CFD006u)
#define pow_dln2 K(0x3FE62E42FEFA39EFu)
#define pow_dln2_32 K(0x3FE62E4300000000u)
#define pow_dln2_32r K(0xBE205C610CA86C39u)
static const uint64_t pow_dCpow[5] = {
    0x3FC555555555553Eu, 0xBF66C16C16BEBD93u, 0x3F11566AAF25DE2Cu, 0xBEBBBD41C5D26BF1u,
    0x3E66376972BEA4D0u,
};
#define cm_dblinf K(0x7FF0000000000000u)
#define cm_hugeval K(0x7FEFFFFFFFFFFFFFu)
#define cm_twom53 K(0x3CA0000000000000u)

/* 2/pi, ten bits at a time (Trig64's pibits) */
static const uint16_t trig_pibits[126] = {
       0,    0,    0,    0,    0,    0,  162,  998,   54,  915,
     580,   84,  671,  777,  855,  839,  851,  311,  448,  877,
     553,  358,  316,  270,  260,  127,  593,  398,  701,  942,
     965,  390,  882,  283,  570,  265,  221,  184,    6,  292,
     750,  642,  465,  584,  463,  903,  491,  114,  786,  617,
     830,  930,   35,  381,  302,  749,   72,  314,  412,  448,
     619,  279,  894,  260,  921,  117,  569,  525,  307,  637,
     156,  529,  504,  751,  505,  160,  945, 1022,  151, 1023,
     480,  358,   15,  956,  753,   98,  858,   41,  721,  987,
     310,  507,  242,  498,  777,  733,  244,  399,  870,  633,
     510,  651,  373,  158,  940,  506,  997,  965,  947,  833,
     825,  990,  165,  164,  746,  431,  949, 1004,  287,  565,
     464,  533,  515,  193,  111,  798,
};

#endif
