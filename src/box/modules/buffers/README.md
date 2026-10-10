# Buffer Manager

A native module in C (`buffers.c`) that replaces the RISC OS 5 Buffer Manager.
It follows ROOL's `Doc/BufferMan`, the routines' entry comments and
`hdr/Buffer`. Names, numbers and register roles are RISC OS's.

A buffer is a ring of bytes in shared memory. `Buffer_Create` takes the memory
from the RMA, and `Buffer_Register` uses memory that the client supplies. The
data is the bytes from `rem` up to `ins`, wrapping at the size. One unit is
always kept free (four bytes for a word-aligned buffer, flag bit 4). Producers
and consumers may run together, using C11 release and acquire atomics.

## SWIs (chunk &42940)

Buffer_Create, Buffer_Remove, Buffer_Register, Buffer_Deregister,
Buffer_ModifyFlags, Buffer_LinkDevice, Buffer_UnlinkDevice, Buffer_GetInfo,
Buffer_Threshold and Buffer_InternalInfo, in that order from &42940. The
registers are typed in `api/defs/buffers.toml`.

Flags: bit 0 not dormant, bit 1 generate Event_OutputEmpty, bit 2 generate
Event_InputFull, bit 3 issue UpCall_BufferFilling and UpCall_BufferEmptying at
the threshold, bit 4 word aligned.

Data moves through InsV (&14), RemV (&15) and CnpV (&16), through the direct
call routine from `Buffer_InternalInfo`, or from C through `buffers.h`
(`xbuffer_put`, `xbuffer_get`, `xbuffer_peek`, `xbuffer_count`,
`xbuffer_purge`). The module issues Service_BufferStarting (&6F).

Errors start at &20700: SWI value out of range, too many buffers, buffer not
known, buffer manager in use, unable to detach current owner, handle already in
use, buffer too small, must be word aligned, and bad parameters.

## Differences from RISC OS 5.30

- A byte inserted into a full buffer is not written into the gap first.
- The direct call routine checks its id and workspace, and returns "Bad
  parameters" for a bad one.
- A block-insert Event_InputFull carries 0 in R2 where the data is ordinary C
  memory.
- The module keeps no state outside the RMA.
- The direct call interface was for DeviceFS drivers, which Linux replaces.

## Tests

`boot/selftest_buffers.c`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
