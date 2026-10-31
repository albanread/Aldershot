# The Game Pane

*31 October 2026*

**Note: this is not RISC OS.** BOX is a RISC OS window manager and
application environment running on a Linux kernel. [Is BOX Linux?](5-Linux.md) says what that means. The game pane is BOX's own,
and is not part of RISC OS.

## Introduction

The game pane is BOX's retro graphics and sound: a window in which a
program draws the way an arcade game of the late 1980s and early 1990s
did, with a palette, scrolling layers, hardware-style sprites and a text
overlay, and plays music and effects on a three-voice synthesiser chip.
None of it is emulated. It is a model of how that hardware worked, drawn
by a modern graphics card, and a synthesiser written from the behaviour
of a famous sound chip.

This article covers:

- the gap the game pane fills;
- the picture, layer by layer, and the hardware each layer stands for;
- the shared block a program writes, and the five SWIs;
- how the picture is drawn, on a PC and on a Mac;
- the sound chip;
- the ABC player, and the effects;
- Galaxigans, the game written to prove it all;
- which languages can use it today;
- the limits.

![The game pane demonstration: the black-hole backdrop, a formation of sprites, copper bars and a palette strip, with the text layer on top](images/gamepane-demo.png)

---

## 1 The gap

The Acorn machines were fast and clean, but their graphics and sound were
plain. The Archimedes' video controller drew the screen from memory, and
its one hardware sprite was the mouse pointer. Its sound was eight
channels of samples, which software had to make. There was no blitter, no
playfields or tile layers to scroll, no sprites for the game's own
objects, and no synthesiser. Everything a game showed, it drew itself, a
byte at a time, with the processor.

The American machines were built the other way. The Atari 800 had a
display processor that ran a list of instructions for each frame, and
players and missiles drawn by the hardware. The Commodore 64 had eight
hardware sprites, smooth scrolling and the 6581 sound chip: a
three-voice synthesiser with an analogue filter, on one chip. The Amiga had a copper, which changed the
display's registers at a chosen line of the screen; a blitter, which moved
and combined blocks of memory while the processor did something else;
two playfields; hardware sprites; and four channels of sampled sound.
These chips are why the games on those machines looked and sounded as
they did.

The game pane gives BOX programs those things. A RISC OS program can now
put eight thousand sprites over a scrolling scene with a different
palette on every line, and play a tune on a 6581-style chip while its
sound effects play on a second one, without drawing a pixel of it itself.

---

## 2 The picture

The pane is 640 by 360 pixels, always. A pane whose size could be set
would be a framebuffer with extra steps; a fixed size is what the old
hardware had, and what a game is designed around. The window shows it at
whatever size it is, each pane pixel a block of screen pixels, with no
smoothing. At 1280 by 720 each pixel is exactly doubled.

It is built from four layers, drawn in this order, bottom to top:

| Layer | What it is | What it stands for |
| --- | --- | --- |
| 0, the backdrop | a picture worked out for every pixel by a shader, one of twelve scenes | the hand-painted backdrops of the arcade, and the raster effects of the demo scene |
| 1, the canvas | 640 x 360 bytes, each a colour number, with its palettes | the playfield, and the copper |
| 2, sprites | up to 8,192 rectangles cut from a sprite sheet | hardware sprites |
| 3, text | up to 4,096 characters from a 5 x 7 font | the score line |

On a PC there are also up to four tile layers, which scroll; they are
described below.

### The backdrop

Layer 0 is drawn first, and fills the whole pane, so it is also the
frame's clear. It is one triangle and one fragment shader, which works out
the colour of every pixel from its position and the time. There are
twelve scenes: nebula, galaxy, black hole, alien world, moon, nova,
wormhole, gas giant, aurora, pulsar, plasma and binary stars. A program
chooses one by number, or none.

The scenes move with the clock, not with the game, so the sky drifts at
the same speed whether the game is running at thirty frames a second or
sixty. They cost the processor nothing.

### The canvas and the copper

The canvas is 230,400 bytes, one for each pixel, and each byte is a
colour number. There are two palettes:

- colours 16 to 255 come from one global palette of 240 entries;
- colours 0 to 15 come from a palette of their own **for each of the 360
  lines**.

The second is the copper. On the Amiga, the copper changed a colour
register as the beam reached a given line, and the same colour number
then showed a different colour further down the screen. That is how the
Amiga drew its famous rainbow bars across a still picture. The game pane
gives a program a table of 16 colours for every line instead, which
comes to the same thing: write the canvas once, then change the table
every frame, and the picture moves without a pixel being redrawn. The
demonstration's bars are drawn this way, as is the tractor beam in
Galaxigans, which is one colour number painted once and animated by
rewriting that colour on each line.

The canvas can be drawn in two ways. Over the backdrop as a layer, colour
0 is a hole and the backdrop shows through. On its own, it is the whole
picture.

### Sprites

Sprites are rectangles cut from a 512 by 512 sprite sheet. Each sprite is
32 bytes in a table: where it is on the pane, where its picture is on the
sheet, its width and height, its colour scheme and its transparency. The
sheet holds colour numbers, so a sprite's colours come from one of 32
schemes of 16 colours, and the same picture can be a red alien and a
green one. Colour 0 is transparent.

A program draws up to 8,192 sprites a frame, and all of them are drawn in
one instruction to the graphics card. Later sprites are drawn over
earlier ones. Sprites are not scaled or rotated: a sprite is the size of
its picture, as a hardware sprite was. A sprite may be drawn solid, or
blended with what is under it by its transparency.

Each sprite is held strictly to its own rectangle of the sheet, so that a
neighbouring frame of animation never bleeds into its edge, which is the
fault every sprite-sheet renderer has to guard against.

### Text

The text layer draws up to 4,096 characters, at any pixel position, each
with its own colour and size. The font is 5 by 7 pixels in an 8 by 8
cell, in capitals, and the pen moves 6 pixels a character. All the text
on the pane is one draw.

### Tile layers

On a PC the pane can also have up to four tile layers, the parallax
scrolling of the 16-bit consoles. A tile layer is a map of 8 by 8 tiles,
up to 1,024 different tiles from a sheet, over a world 1,024 pixels wide.
Each layer scrolls by itself. Sideways it wraps round for ever, at its
own width, so that layers of different widths drift past one another;
upwards and downwards it stops at the world's edge. A game can ask which
tile is under a point. The tile layers are not yet in the Mac's version
of the pane.

### No blitter

The original game pane, written in Mojo for Windows, had a blitter: a
bank of eight planes and four operations between them (copy, copy with a
transparent colour, AND, OR or XOR, and fill), "the Amiga's blitter" in
its own words. BOX's pane does not need one. A blitter existed to move
pictures around a framebuffer faster than the processor could. In the
game pane the graphics card composes every layer afresh each frame, so a
sprite is moved by changing two numbers in its table.

---

## 3 The block and the SWIs

On BOX a program draws by writing a block of memory, about 900 KB, laid
out as the hardware's registers and memory would be:

| Part | Size |
| --- | --- |
| a header: the frame number, the backdrop's scene and time, the counts | 64 bytes |
| the canvas | 230,400 bytes |
| the global palette | 1,024 bytes |
| the line palettes | 23,040 bytes |
| the sprite table | 262,144 bytes |
| the text table | 131,072 bytes |
| the sprite sheet | 262,144 bytes |
| the sprite colour schemes | 2,048 bytes |
| the font | 8,192 bytes |

The block is shared with whatever draws the picture. A frame number in
the header keeps the two in step. It is odd while the program is writing
and even when the frame is finished, so the picture is never drawn from
a half-written frame; if the drawer finds it odd, it shows the last
frame again. The sprite sheet, the schemes and the font are large and
rarely change, so they carry a number of their own and are copied to the
graphics card only when it changes. An ordinary frame copies only the
canvas, the palettes and the two tables.

The drawer trusts nothing in the block: it checks the block's mark and
version, and limits every count and every index, so a program with a bug
draws a wrong picture, not a crash.

The GamePane module gives the block to a program:

| SWI | Number | What it does |
| --- | --- | --- |
| GamePane_Open | &C0280 | makes a block for a window and binds it to the window; returns its address and size |
| GamePane_Begin | &C0284 | marks the frame as being written |
| GamePane_Present | &C0281 | marks the frame as finished, for the drawer to show |
| GamePane_Drawn | &C0283 | says whether the host is drawing this pane |
| GamePane_Close | &C0282 | unbinds the block and frees it |

A frame is Begin, the writes, then Present. The block is carved from the
memory BOX keeps for windows' pictures, and bound to the window as a kind
of surface; a window may have one, and the box up to four.

---

## 4 How it is drawn

The pane is drawn by the graphics card, in one of two ways.

**On a PC**, and on the Intel Macs that run BOX directly, the pane is a
Linux program drawing with OpenGL ES through the machine's own Mesa
driver. It is a Wayland client, and BOX's compositor makes every Wayland
window a Wimp window, with its furniture. The compositor takes the
finished frame from the graphics card's memory as it is, so the picture
never passes through the processor. Frames are timed to the display, at
60 frames a second on most panels. The pane's drawing thread asks for
real-time priority, and so does its sound, at a higher priority, because
a late frame is a glitch and a late sound buffer is a click.

**On a Mac**, the Apple silicon box runs under QEMU, and the program in
the box draws nothing at all. It writes the block, and the Mac's own
graphics, Metal, read the block from the box's memory and draw the four
layers into a 640 by 360 picture, which is then shown in the window. The
box's processors spend their time on the game alone: Galaxigans runs at
60 frames a second for 7 per cent of one of the box's processors.

The two are written to the same description. Every layer of the PC's
version was proved by a test that draws it without a screen, reads the
pixels back and checks every one against a model worked out by the
processor: all 230,400 pixels of the canvas, twelve scroll positions of
the tile layers, 450 sprites, 133 characters in six colours. On the 2018
Mac mini, with its own Intel graphics, the pane runs at 90 frames a
second; on the 2013 Mac Pro's AMD card, at about 60.

---

## 5 The sound chip

The pane's sound is a model of the 6581, the Commodore 64's sound chip.
It is not an emulation of the 6581's circuits. It is a synthesiser that
does what the 6581's registers say, written from the way the chip
behaves, in integer arithmetic except for its filter.

| Part | What it does |
| --- | --- |
| Voices | three, each an oscillator of 24 bits clocked as a PAL Commodore 64 was, at 985,248 Hz |
| Waveforms | triangle, sawtooth, pulse with a 12-bit width, and noise; several chosen together are ANDed, as the chip did |
| Noise | a 23-bit shift register, with the chip's own feedback taps and the eight bits it took its output from |
| Envelope | attack, decay, sustain and release, from the chip's own table of times, 2 ms to 8 seconds; attack is a straight line, and decay and release slow down as the level falls, as the 6581's did |
| Ring modulation and sync | each voice can take the one before it as the source: ring modulation for bells and gongs, hard sync for the tearing lead sounds |
| Filter | low-pass, band-pass and high-pass, alone or together, with a cutoff and a resonance; any voice can be sent through it |
| Output | 48,000 samples a second, the player stepping 50 times a second, as Commodore 64 music did |

The envelope is the part most synthesisers get wrong. The 6581 did not
fade a note out smoothly: as the level fell, its counter was stepped by a
larger and larger divisor, so the tail of a note stretched. That stretch
is a large part of the chip's sound, and the model has it.

There are **two chips**, one for music and one for sound effects. On the
Commodore 64 a game had three voices for everything, and a shot stole a
voice from the tune; here the effects have their own three voices, so a
shot never cuts the music. The two are mixed and played by a thread of
their own.

On a PC the sound goes to Linux's own sound system, shared with the web
browser. On the Mac it goes through a loopback sound card inside the box,
whose other end BOX's mixer reads, so it comes out of the Mac with the
rest of the box's sound.

---

## 6 Music: the ABC player

Tunes are written in ABC, the plain-text music notation folk musicians
have used since the 1990s:

```
X:1
T:Scale
M:4/4
L:1/8
Q:1/4=120
K:G
[I:chip v=0 wave=pulse pw=500 a=0 d=4 s=6 r=3 vol=12]
GABc defg|gfed cBAG|
```

The player reads the note length (`L:`), the tempo (`Q:`) and the key
(`K:`, any major or minor key, its sharps and flats worked out from the
circle of fifths). It reads notes over the octaves, sharps, flats and
naturals that last to the bar line, rests, and lengths written as
multiples and fractions. Up to eight voices (`V:`) play at once, each
with its own place in the tune, on the chip's three.

The `[I:chip ...]` field is BOX's own. It sets a voice's sound from
inside the tune: the waveform, the pulse width, the envelope, the filter
and its cutoff, resonance and mode, the volume, ring modulation and sync.
A setting the player does not know is refused by name, not ignored.

The player keeps time in ticks, 480 to a crotchet, and puts every note on
at the exact sample it falls on, not at the next 50th of a second, so a
tune does not drift. Each note is released a little before its end, so
the envelope starts again on the next note, as a player's fingers would.

Sound effects are not tunes. Each is a recipe of chip settings with a
pitch that falls fifty times a second: a shot, an explosion, a dive, a
pick-up and the player's death. They play on the effects chip, a voice
each in turn.

---

## 7 Galaxigans

Galaxigans is the game written to prove all this. It is a Galaxian-style
shoot-'em-up: fourteen species of alien in formation, which peel off and
dive at the player's ship; a bonus saucer with a spinning mine; on every
third wave a capture boss with a tractor beam; a victory dance; and a
hall of fame. It has twelve levels, each with its own backdrop; the
colour themes come round every six.

![Galaxigans in the Apple silicon box: wave 1, the nebula, the formation and the player's ship](images/gamepane-galaxigans.png)

Its history is a chain of translations, as so much of BOX is. It began
as a game in x86-64 assembler, was ported to Smalltalk in MACVM, then to
Mojo on Windows, and then to C for BOX. None of its art was retyped on
the way. A script reads the sprites from the Mojo source, which had them
from the Smalltalk, which had them from the assembler: 46 sprites, among
them two frames for each of the fourteen species, eight turns of the
mine and four poses of the boss. Another script takes the eighteen tunes
from the Mojo: a motif for each species, played as it dives, and cues for
the title, the end of a stage, the saucer and the aliens' victory. A third
translates the twelve backdrop shaders into the language of each graphics
card, and checks that all 411 numbers in them came across unchanged and
in order.

It runs at a steady 30 steps a second, whatever the frame rate. In BOX it
is `*Galaxigans`, or `*Galaxigans -autoplay` to let the computer play;
Left and Right steer, and Space fires. It is also !Galaxigans on the
disc.

---

## 8 Which languages can use it

| Language | How |
| --- | --- |
| C | in the ROM, through the GamePane SWIs and the block: the demonstration and Galaxigans are written so. On a PC, as a Linux program using the pane's own library |
| Lisp | `(require 'gamepane)` and `(require 'sound)` in !Lisp: `gp-open`, `gp-rect`, `gp-line`, `gp-colour`, `gp-line-colour` for the copper, `gp-scene`, `gp-sprite`, `gp-text`, `gp-present`; `snd-tune`, `snd-music` and `snd-effect`. The pane is a surface in a Declaritive window |

In Lisp, a pane and a tune are a few lines:

```lisp
(gp-open window "pane")
(gp-colour 16 255 200 0)
(gp-rect 100 100 50 50 16)
(gp-present)
(snd-start)
(snd-music (snd-tune "X:1
T:Scale
M:4/4
L:1/8
K:C
CDEF GABc|") nil)
(snd-effect :shot)
```

The Lisp examples include Conway's Game of Life on a game pane, a ball
bouncing over the black-hole backdrop, and a jukebox of chip tunes.

---

## 9 Limits

- The block is written by programs in the ROM, and by !Lisp. BBC BASIC,
  Lua, Python, Mojo and Smalltalk have no binding to it yet; it is a
  block of memory and five SWIs, so each is a short piece of work.
- On a Mac, only the first visible game pane is drawn; a second pane's
  window stays blank.
- The tile layers are drawn on a PC only.
- `GamePane_Open` needs a host to draw the block, which the Mac has. On a
  PC the demonstration and Galaxigans run their Linux versions instead.
- Sprites are not flipped, scaled or rotated.
- The font has capitals only.
- The ABC player does not yet expand repeats, or play chords, triplets,
  ties or grace notes; the Mojo original it came from does, and they are
  to follow.
