# Ancient and Modern

*11 October 2026*

**Note — non-technical opinion.** The other articles here describe how the
code works and can be checked against it. This one is a point of view about
old software and new machines, and nothing in it should be taken as a
description of the system.

## Introduction

I value software for its cultural significance. Some of my favourite
software is thirty years old. It was written by people working under
constraints we no longer have, and it shows: it is small, it is quick, and
it was finished.

The question this article is about is how to keep such software, and the
experience of using it, when the machines it was written for have gone. My
answer is that the way to preserve the ancient is to go modern. That sounds
like a contradiction. I think it is the opposite.

---

## 1. Preserving the experience, not the bytes

A program kept as a file is not preserved in any useful sense. Nobody can
run it. What is worth keeping is the experience of using it: the program
responding as it did, on a screen, to a keyboard and a mouse.

That means the program has to run, and to run it needs everything under it.
On the old machines that was a processor, an operating system and a set of
libraries, all gone or going. So the choice is to keep the whole stack
working somehow, or to move the program.

Moving it is the more honest answer, and it can be done properly. BCPL can
be translated into Rust. Strongtalk can be made to run on Apple silicon.
These are not emulations: the program becomes a current program, built with
current tools, which a current compiler will complain about and a current
debugger will step through. It keeps its behaviour and loses its
dependence on hardware that no longer exists.

That is the work this project does with RISC OS. The operating system is
translated into C, and the C is compiled for the machine in front of you.
What is preserved is not the ARM instructions; it is what the system does.

---

## 2. Retro-futurism, and why it is a story

There is a pleasure in imagining the other history. Acorn does not go
under. Commodore does not go under. The beige boxes, dull beyond
description, do not inherit the earth. The machines of 2026 are the
descendants of the Archimedes and the Amiga, and they are strange and
quick and good-looking.

It is an attractive vision and I understand its appeal. It is also
steampunk: a present built out of a past that did not happen. Steampunk is
a design language, not a prediction, and this is the same. It tells you
what people wanted, not what was going to occur.

---

## 3. What would have happened anyway

The honest view is that AmigaOS, RISC OS and the Lisp machines would have
been replaced whatever had happened to their companies. They would have
been replaced by Unix, and in practice by Linux.

Terry Pratchett and Neil Gaiman put the mechanism better than I can,
though they were writing about cassettes:

> all tapes left in a car for more than about a fortnight metamorphose
> into Best of Queen albums

Leave a computing platform alone long enough and it becomes Linux. Not
because Linux won an argument, but because it is what accumulates. It is
free, it runs on everything, everyone has already paid the cost of
learning it, and every new piece of hardware arrives with a Linux driver
because that is what the manufacturer was given. The cassette in the glove
box is not chosen. It is simply what is there after a while.

---

## 4. RISC OS was lucky, and then unlucky

RISC OS had better luck than its contemporaries, and it was luck of a
specific kind: its processor became popular. ARM went from being Acorn's
own design to being in more devices than anything else on earth. An
operating system written for ARM therefore had hardware to run on for
decades after the company that wrote it had stopped mattering. Nothing
comparable happened to the Amiga.

The bad luck sat inside the good. ARM became popular for low power and for
mobile telephones, not for desktops. So for twenty years the hardware
RISC OS could run on natively was hardware designed to be small and frugal,
which meant slow. And by the time ARM did become genuinely desktop class,
32-bit had been left behind. The processors that could have run RISC OS
fast were the ones that had stopped being able to run it at all.

So RISC OS got a long run of native hardware, which is more than most
systems of its generation got, and that hardware was modest for nearly all
of it. That position is returning now: the machines a 32-bit RISC OS can
run on natively are again the slow ones, and the fast ones are out of
reach. The good luck runs out in the end.

And then the slowness turns into a sort of virtue. Software written to be
quick on a modest machine is small, and it does not waste anything, because
it could not afford to. Move it onto a current processor and it does not
merely work; it goes like a scalded cat. The thirty-year-old program gets
the benefit of thirty years of silicon, and it was built lean by people who
had no choice in the matter.

That benefit is worth more now than it has been for a long while. Computers
sat in a doldrum for years — Intel's doldrum, mostly — where each new
generation was a few per cent on the last and the fan got louder. That has
ended. Every new Mac is quicker than the previous generation's Pro model.
The Snapdragons are on the prowl. Even Intel is competing again, which it
had not needed to do for a decade. None of it is down to any one company's
cleverness: it is down to the extraordinary engineering of the foundries in
Taiwan, on which all of them depend.

---

## 5. Everything is Unix now

It is worth being plain about where the world got to. The watch on your
wrist runs a variant of Unix. So does your telephone. Windows runs Linux in
a virtual machine because people need it to. Containers are Linux. The
cloud is Linux. The build machine, the test machine and the thing that
serves this article are all Linux.

This is not a complaint and it is not a prediction; it is the floor. Any
plan for running old software that begins by rejecting it is a plan for
running old software on a machine nobody has.

---

## 6. The modern is inevitable, the software need not be

Those two facts fit together more comfortably than they look.

The modern layer is inevitable. The processor will be whatever is fast, the
kernel underneath will be Linux, and the hardware will be driven by code
written this year. There is no use arguing with it.

But software can run anywhere. That is the whole point of writing it in a
language rather than in machine code — and a macro assembler counts as a
language. RISC OS is the proof of it. It was written in ObjAsm, which has
symbolic names, macros, conditional assembly and structure, so the source
says what the code means and not merely what bytes to emit. That is why it
can be read by a program and turned into C at all. Had it been hand
assembled, there would be nothing to translate and no way to recover the
intent.

If the program is moved properly — translated, recompiled, kept honest
against its old behaviour — then what runs on that inevitable modern layer
can be as ancient as you like.

So BOX is a Linux kernel with RISC OS on top, and RISC OS is C now. The
bottom is as modern as I can make it, because that is where the speed and
the drivers are. The top is as old as it ever was.

---

## 7. A confession

I should say why I do any of this, because it would be easy to dress it up
as preservation work for the public good.

The only reason I write software is to run it on my own machines. That is
all. I am not building a product, I am not serving a user base, and I have
no plan for either. I want these programs on the computers I actually own,
working at the speed those computers are capable of, and the work described
in these articles is what that turned out to require.

If it is useful to anyone else, that is a side effect, and a welcome one.
But the motive is selfish and I would rather say so.

---

## 8. The best native experience, and how old it is

It is worth putting a date on the hardware, because it makes the point of
this article better than any argument.

The best native experience of RISC OS — the quickest machine the real
32-bit system runs on as itself, with no translation and no emulation —
is a Cortex-A72. Arm announced that core in February 2015, so the design
work on it dates to roughly 2013 and 2014. It was done at Arm's Austin
design centre under the codename Maya, as the successor to the Cortex-A57,
and the aim was not a new architecture but better performance for the same
watt on the ARMv8-A they already had.

The first chips using it reached telephones in early 2016. The Raspberry Pi
4 took it in 2019, by which time it was a mature and inexpensive core, which
is exactly why it was available to a board at that price.

So the fastest native RISC OS machine is built on a design that is about a
dozen years old, and was a deliberately frugal design when it was new. That
is not a complaint about the Pi, which is a fine machine and the reason
RISC OS has had anything to run on at all. It is the whole situation in one
fact: the system's native hardware ceiling was set by a mobile telephone
core from the middle of the last decade, while the machines on this desk
have moved on by a decade and several process nodes.

Which is the argument for doing the work described in these articles. The
ancient software is not the limit. The limit is insisting that it keep its
original instruction set.

---

## Credit

The observation about cassettes is from *Good Omens* (1990) by Terry
Pratchett and Neil Gaiman. It is widely misattributed, including by me
until I checked.
