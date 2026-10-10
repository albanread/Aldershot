/* Copyright 2012 Castle Technology Ltd
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
 * This file is a translation into C of RISC OS Open's International service
 * constants (Sources/Internat/Inter: hdr.Internatio).
 */

/* international.h -- Service_International (&43) and its reasons, as
 * hdr/Internatio has them: the kernel (OS_Byte 70 and 71, the font
 * resets OS_Byte 20 and 25: runtime/keyboard.c, runtime/vdu/vdu.c) and
 * the modules that ask (InternationalKeyboard, FontManager) or answer
 * (modules/international) share them.
 */
#ifndef ROSGD_INTERNATIONAL_SERVICE_H
#define ROSGD_INTERNATIONAL_SERVICE_H

#define SERVICE_INTERNATIONAL   0x43u
enum {
    INTER_CNA_TO_CNO = 0,       /* country name to number */
    INTER_ANA_TO_ANO = 1,       /* alphabet name to number */
    INTER_CNO_TO_CNA = 2,       /* country number to name */
    INTER_ANO_TO_ANA = 3,       /* alphabet number to name */
    INTER_CNO_TO_ANO = 4,       /* country number to its alphabet */
    INTER_DEFINE = 5,           /* define an alphabet's characters in the system font */
    INTER_KEYBOARD = 6,         /* the keyboard has changed (never claimed) */
    INTER_DEFINE_UCS = 7,       /* define a character from a UCS code */
    INTER_UCS_TABLE = 8,        /* an alphabet's table of UCS codes */
    INTER_ISO3166_ALPHA2 = 9,   /* a country's ISO 3166-1 alpha-2 code */
    INTER_HIGHEST = 10,
};

#endif
