/* Copyright 2019 RISC OS Open Ltd
 * Copyright 2020 RISC OS Open Ltd
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
 * This file is a reimplementation in C of RISC OS Open's CompressPNG module
 * (Sources/Video/Render/CompressPNG: c.compresspng, c.memory, c.module, h.CompressPNG,
 * h.CompressPNG_int).
 */

/* compresspng.h -- CompressPNG (Video/Render/CompressPNG, 0.07), as a
 * native module over libpng. Its four SWIs are the original's c/compresspng,
 * call for call. Its memory is a dynamic area for each compression, as in the original.
 */
#ifndef ROSGD_COMPRESSPNG_H
#define ROSGD_COMPRESSPNG_H

extern struct ros_module compresspng_module;

/* The flags for CompressPNG_Start (h/CompressPNG). */
#define COMPRESSPNG_TO_FILE    1u
#define COMPRESSPNG_HAS_ALPHA  2u
#define COMPRESSPNG_GREYSCALE  4u
#define COMPRESSPNG_SKIP_ALPHA 8u

/* hdr/NewErrors: ErrorBase_CompressPNG. The error codes of h/module are added to it. */
#define COMPRESSPNG_ERRBASE    0x821600u

#endif
