# Articles

Technical articles about BOX (RISC OS on a Linux kernel, translated to
C). They follow the style of the RISC OS Programmer's Reference Manuals
and describe the code as it currently works. Each article can be read on
its own.

| Written | Article | What it covers | Read |
| --- | --- | --- | --- |
| 3 October 2026 | **BOX Tools** | The tools that make software for BOX, all of which write or compile C: x32 and A64X32 (64-bit code with 32-bit pointers), `*CC` (clang in the box), ROSASM (ObjAsm to C), ROSBAS (BBC BASIC V to C) and the roscc linker. | [BOX Tools](box-tools.md) |
| 3 October 2026 | **Tiers of Translation** | The four levels of translated code: tier 0 (unlifted, exact, slow), tier 1 (lifted by the compiler), tier 2 (rewritten from a specification) and tier 3 (made readable), with examples and timings. | [Tiers of Translation](tiers-of-translation.md) |
| 3 October 2026 | **BOX Architecture** | Overview of the system: what the Linux kernel provides, the RISC OS personality, the arena (where a RISC OS address is a host address), how tasks run as threads, and how SWIs are called. | [BOX Architecture](box-architecture.md) |

The newest article goes at the top.
