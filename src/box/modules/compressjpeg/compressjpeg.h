/* Copyright RISC OS Open Ltd and others
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
 * This file is derived from RISC OS Open's source and from other code.
 * The Apache licence of RISC OS Open's source applies to it.
 */

/* compressjpeg.h -- CompressJPEG (Video/Render/JCompMod, 0.08), as a
 * native module over libjpeg-turbo. Its four SWIs are listed in README.md
 * in modules/compressjpeg.
 */
#ifndef ROSGD_COMPRESSJPEG_H
#define ROSGD_COMPRESSJPEG_H

extern struct ros_module compressjpeg_module;

/* c/jcompmod's ErrorBase_CompressJPEG. */
#define COMPRESSJPEG_ERRBASE 0x8183C0u

#endif
