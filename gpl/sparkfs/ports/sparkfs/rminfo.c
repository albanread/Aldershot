/* rminfo.c -- AsmUtils' Image_RO_Base (Lib/AsmUtils, s/rminfo), in C.
 *
 * The ObjAsm exports a word holding |Image$$RO$$Base|, the module's base:
 * SparkFS gives FileSwitch its entries as offsets from it (AddFS, AddImageFS
 * want offsets from the module's start).  roscc places a module's header,
 * cmhg's __cmhg_module, at offset 0, and relocates this word with the
 * module, so it is the base wherever the module is loaded.
 *
 * Apache 2.0, as AsmUtils (Copyright 1999 Pace Micro Technology plc).
 */
extern const char __cmhg_module[];

unsigned long Image_RO_Base = (unsigned long)__cmhg_module;
