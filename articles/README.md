# Articles

Technical articles about BOX. They follow the style of the RISC OS
Programmer's Reference Manuals and describe the system as it works
today. Each article can be read on its own.

| Article | What it covers |
| --- | --- |
| [BOX Design](0-Design.md) | How BOX is put together: the Linux kernel and the RISC OS personality; the arena, the low 4 GB in which a RISC OS address is a host address; where the two meet, service by service — screen, pointer, input, sound, filing systems, network, time, faults, threads, parallel work, the Unix bridge and the ARM container; tasks as threads, the personality lock and the baton; and how a SWI is made. |
| [Tiers of Translation](1-Tiers.md) | How ARM assembler becomes C, and at what cost: the four tiers — tier 0 one C statement per instruction, exact by construction; tier 1 lifted into structured C and tested against tier 0; tier 2 rewritten by hand from a specification; tier 3 rewritten by hand against the translation and tested against it. What decides correctness at each, worked examples, timings, and how a module's tier is chosen. |
| [Emulation in BOX](2-Emulation.md) | Is BOX an emulator? No, but it contains one. What a full emulator costs; why BOX's operating system is native and only an ARM program's own instructions are emulated; the ARM container — dynarmic, two back ends, memory without marshalling, native floating point; ARM modules as shadows of native ones; an emulated program as an ordinary Wimp task; what runs today, honestly counted; what it will not do, and when to use a full emulator instead. |
