# dynarmic: BOX's patches

BOX runs ARM programs and modules in its ARM container, which uses
dynarmic, a dynamic recompiler for ARM code. This directory holds the
changes BOX makes to it. The source of dynarmic itself is not here: it
is dynarmic's, and is fetched by the build.

How BOX uses dynarmic, and why each change was needed, is described in
the article [Dynarmic in the BOX](../../../../articles/7-Dynarmic.md).

## The source

| Component | Where | Commit | Licence |
| --- | --- | --- | --- |
| dynarmic | Azahar's fork, `github.com/azahar-emu/dynarmic` | `a46601580d5512d324104f985b5f0209dc980ddc` | 0BSD |

dynarmic's externals (mcl, oaknut, xbyak, robin-map, fmt, zydis and
zycore) are taken at the commits its submodules name, unchanged. Boost
1.89.0 is used for its headers only.

## Applying the patches

Check out the commit above, with its submodules, and apply the patches
in order:

    git clone https://github.com/azahar-emu/dynarmic
    cd dynarmic
    git checkout a46601580d5512d324104f985b5f0209dc980ddc
    git submodule update --init
    for p in /path/to/patches/*.patch; do git apply "$p"; done

Patch 0001 takes effect only when the code is compiled with
`-DDYNARMIC_EXTERNAL_SIGNALS`. BOX builds the A32 front end only, with
the AArch64 back end on Apple silicon and the x86-64 back end on Intel.
Each place a patch changes is marked `ROSGD (runtime/armrun/patches/000N)`.

## The patches

| Patch | What it does |
| --- | --- |
| [0001-external-signals](patches/0001-external-signals.patch) | dynarmic installs no signal handlers and no signal stack; the program's own handler asks `dynarmic_handle_fault` whether a fault was in translated code. |
| [0002-invalidate-every-block](patches/0002-invalidate-every-block.patch) | Invalidating an address range removes every translated block it covers, not only the first one found. |
| [0003-raise-instead-of-interpret](patches/0003-raise-instead-of-interpret.patch) | Instructions upstream would hand to an interpreter raise an exception to the embedder instead. |
| [0004-undefined-coprocessor-instructions](patches/0004-undefined-coprocessor-instructions.patch) | A coprocessor instruction that no coprocessor accepts is an undefined instruction. |
| [0005-single-step-after-a-raise](patches/0005-single-step-after-a-raise.patch) | Single-stepping an instruction that raised an exception steps that instruction correctly. |
| [0006-memory-abort-check-32-bit](patches/0006-memory-abort-check-32-bit.patch) | The AArch64 back end's memory-abort check reads the halt word as 32 bits. |
| [0007-msr-control-field](patches/0007-msr-control-field.patch) | `MSR` writing the control field (a change of processor mode) raises to the embedder. |

## Licence

dynarmic is under the 0BSD licence. These patches are offered under the
same licence.
