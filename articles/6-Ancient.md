# Ancient and Modern

*11 October 2026*

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
reach.

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
language rather than in machine code. If the program is moved properly —
translated, recompiled, kept honest against its old behaviour — then what
runs on that inevitable modern layer can be as ancient as you like.

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

## Credit

The observation about cassettes is from *Good Omens* (1990) by Terry
Pratchett and Neil Gaiman. It is widely misattributed, including by me
until I checked.
