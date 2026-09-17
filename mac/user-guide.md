# RISC OS for the Mac — user guide

This guide covers **RISCOSQEA72v1**, the Mac application that runs
RISC OS 5.30 in a window on your Mac. It is written for someone who wants
to use RISC OS, not build it.

- [What you need](#what-you-need)
- [Installing](#installing)
- [The first run: choosing the disc folder](#the-first-run-choosing-the-disc-folder)
- [The mouse and the keyboard](#the-mouse-and-the-keyboard)
- [The window](#the-window)
- [Networking](#networking)
- [Screen sizes](#screen-sizes)
- [Your files: the disc folder](#your-files-the-disc-folder)
- [Switching off](#switching-off)
- [Settings RISC OS remembers](#settings-risc-os-remembers)
- [Moving the disc, or starting again](#moving-the-disc-or-starting-again)
- [What is on the disc](#what-is-on-the-disc)
- [When something goes wrong](#when-something-goes-wrong)
- [Removing it](#removing-it)
- [Licences](#licences)

---

## What you need

**A Mac running macOS 26** or later. That is the one real requirement:
the app runs on any Mac that runs macOS 26, with Apple silicon or with an
Intel processor. Earlier versions of macOS will not open it.

To check your Mac, choose **Apple menu › About This Mac**. It shows the
chip, Apple M-something or Intel, and the macOS version.

Nothing else is needed. RISC OS itself is inside the app: RISC OS Open's
5.30 Raspberry Pi ROM, with our HostFS filing system added, and a minimal
disc for it to boot from.

## Installing

1. Download the disk image for your Mac from the
   [release page](https://github.com/albanread/Aldershot/releases/tag/RISCOSQEA72v1):
   **[RISCOSQEA72v1.dmg](https://github.com/albanread/Aldershot/releases/download/RISCOSQEA72v1/RISCOSQEA72v1.dmg)** for Apple silicon, or
   **[RISCOSQEA72v1-x86_64.dmg](https://github.com/albanread/Aldershot/releases/download/RISCOSQEA72v1/RISCOSQEA72v1-x86_64.dmg)** for an Intel Mac.
2. Open it, and drag **RISCOSQEA72v1** onto the **Applications** folder
   beside it.
3. Open the app from Applications.

The app is signed and notarised by Apple. The first time you open it,
macOS asks whether you are sure you want to open an app you downloaded
from the internet. Click **Open**. It does not ask again.

## The first run: choosing the disc folder

RISC OS keeps its disc in an ordinary folder on your Mac. The first time
the app runs it asks where that folder should be:

- **Use /Users/you/RISCOS** makes a folder called RISCOS in your home
  folder. This is the easy choice.
- **Choose a Folder…** lets you pick somewhere else. A new, empty folder
  is best, because it becomes the disc. If the folder already holds
  files, they are left alone: nothing of yours is overwritten.

The app then copies the RISC OS disc into that folder and starts. On an
Apple silicon Mac it takes about twenty seconds from here to a desktop
you can use. You will see the
boot screen, with its progress bar, and then the desktop.

The app remembers the folder. Later versions of the app never replace a
disc that is already there, so your files and settings carry over.

## The mouse and the keyboard

RISC OS uses three mouse buttons, and a Mac mouse or trackpad has one.

| RISC OS button | What it does | On a Mac |
| --- | --- | --- |
| **Select** | The normal click | Click |
| **Menu** | Opens a menu wherever you click | **Control**-click |
| **Adjust** | The "other" click, often the opposite of Select | **Command**-click, or **Option**-click, or **Shift**-click |

RISC OS has no menu bar at the top of the screen. Every menu comes from
Menu-clicking on the thing you want a menu for: a window, an icon, the
icon bar.

**The keyboard is set up as a UK keyboard.** On a US Mac keyboard most
keys are where you expect, but a few symbols are not. For example,
Shift-2 types `"` and Shift-' types `@`.

## The window

The app's own menu bar, at the top of the Mac screen, has these commands.

| Command | Keys | What it does |
| --- | --- | --- |
| Quit RISC OS | ⌘Q | Switches RISC OS off and closes the app |
| Grab Pointer | ⌃⌥G | Gives the pointer to RISC OS; press again to take it back |
| Save Screenshot | ⌘S or F13 | Saves the RISC OS screen as a PNG |
| Load Snapshot | | Rewinds the machine to the desktop as it was when the last snapshot was saved |
| Backdrop | | Chooses what the Mac draws behind the RISC OS desktop: the Acorn scenes and wallpapers, or your own pictures |
| HostNet | | Networking mode — see [Networking](#networking). Ticked, your Mac serves RISC OS's network; choosing it again switches to RISC OS's own networking and restarts |
| Toggle Full Screen | ⌃⌘F | Fills the screen with RISC OS, or returns to the window |

Screenshots are saved in `~/Library/Application Support/RISCOSQEMU/`.

You rarely need to grab the pointer: the pointer moves in and out of the
window freely. A middle click, on a mouse that has one, also grabs it.

## Networking

Networking works from the first boot, with nothing to configure: open
NetSurf and browse. RISC OS asks your Mac to make its connections for
it, so the web works the way it does on your Mac — same network, same
firewall, same permissions. This is **HostNet**, and it is the default
(the ticked item in **Machine > HostNet**).

**Machine > HostNet** switches to **RISC OS's own networking** instead:
the stack RISC OS has always used, running inside RISC OS over an
emulated network card. It asks first, because switching restarts the
machine — anything not saved is lost, as it is when you switch a real
machine off. Choose HostNet again to come back the same way.

Which should you use? HostNet, almost always: it is why the web works
out of the box. RISC OS's own stack is the original arrangement, kept
for the things that want a real network interface inside RISC OS — but
under emulation its address negotiation was never dependable ("often
worked", and often did not), which is why HostNet exists. If the web
stops working in that mode, that is the one you are in.

## Screen sizes

RISC OS starts at 800×600. To change it, click the monitor icon near the
right-hand end of the icon bar. In the window that opens, pick a
**Resolution** and click **Change**. The sizes on offer are:

640×480 · 800×600 · 1024×768 · 1280×720 · 1280×800 · 1280×1024 ·
1440×900 · 1600×1200 · 1920×1080 · 1920×1200

The window changes size to match. You can also drag the window to any
size you like, and the picture scales to fit.

**On a Retina screen the window opens small.** The app gives each RISC OS
pixel one pixel of your screen, so 800×600 is a small window. Drag it
bigger, or choose a larger size such as 1920×1200, which suits a MacBook
screen well.

To start at a particular size every time, type this in Terminal:

```bash
defaults write com.github.albanread.RISCOSQEA72 mode 1920x1200
```

## Your files: the disc folder

The disc folder **is** the RISC OS disc. On the RISC OS icon bar it is
the **HostFS** disc icon, bottom left. Click it to open the disc.

- Put a file in the folder with the Finder, and it is inside RISC OS.
- Save a file in RISC OS, and it appears in the folder on the Mac.
- Drag files between Filer windows in RISC OS as you would on any disc.

RISC OS names files differently from a Mac, so the names change a little
on the way across:

| On the Mac | In RISC OS |
| --- | --- |
| `notes.txt` | `notes/txt`, a Text file. A dot becomes a slash, because RISC OS uses the dot to separate folders. |
| `Hello,ffb` | `Hello`, a BASIC file. A comma and three hex digits give the RISC OS file type. |
| `picture.png` | `picture/png`, a PNG file. Common extensions give the file type. |
| `README` | `README`, a Text file |

Going the other way, RISC OS adds a `,xxx` type to the Mac name only when
the name alone would give the wrong type. A BASIC program saved as
`Hello` appears on the Mac as `Hello,ffb`.

Letter case does not matter to RISC OS: `Notes` and `notes` are the same
file.

## Switching off

**Close the window, or press ⌘Q.** Either one switches RISC OS off
properly. Please do not Force Quit the app unless it has stopped
responding: that is like pulling the plug on a real computer.

## Settings RISC OS remembers

Changes you make in RISC OS's **Configure** application are saved in the
disc folder, in a file called `CMOS,ff2`, and used the next time RISC OS
starts.

To put every setting back as it was when you installed, quit the app,
delete `CMOS,ff2` from the disc folder, and open the app again.

## Moving the disc, or starting again

**To keep the disc somewhere else:**

1. Quit the app.
2. Move the disc folder in the Finder to where you want it.
3. Hold down **Option** while you open the app. It asks for the disc
   folder again; choose the folder in its new place.

**To start again with a fresh disc:** quit the app, then move the disc
folder to the Trash, or rename it. The next time you open the app it
tells you the folder is not there, asks where the disc should go, and
installs a fresh one.

## What is on the disc

The disc is a cut-down copy of RISC OS Open's own. It has:

- **Applications**: NetSurf (a web browser), StrongED (a text editor),
  PipeDream, Ovation Pro, Maestro, SciCalc and others, in the Apps folder.
- **Utilities**: SparkFS, ChangeFSI, PrivatEye, Snapper, InterGif and more.
- **Printing** tools.

Draw, Paint, Edit, Alarm, Chars, Help and Configure are built into
RISC OS itself: click the **Apps** icon on the icon bar to find them.

Pinned to the desktop are the Welcome page, Configure, NetSurf, StrongED
and ePic.

It does not include RISC OS developer tools, the app store, the manuals
or the games, to keep the download small.

NetSurf reaches the internet through your Mac's connection — see
[Networking](#networking). Sound comes out of your Mac's speakers; use
the Mac's volume control, because the one in RISC OS does not do
anything.

## When something goes wrong

- **The app does not open at all.** Check that your Mac runs macOS 26 or
  later, and that you downloaded the disk image for its processor: Apple
  silicon or Intel.
- **It asks for the disc folder every time.** The folder it was using has
  been moved, renamed or deleted, or is on a drive that is not connected.
  Choose it again.
- **The web does not load.** If **Machine > HostNet** has no tick beside
  it, RISC OS is running its own networking, which is the unreliable
  one — choose HostNet (it restarts the machine) and try again.
- **Something else.** The app writes a log of each run to
  `~/Library/Logs/RISCOSQEA72/`. `run.log` is the main one. If you report
  a problem, include it.

## Removing it

1. Drag **RISCOSQEA72v1** from Applications to the Trash.
2. If you no longer want your RISC OS files, move the disc folder to the
   Trash too.
3. To remove what the app remembers, type these in Terminal:

```bash
defaults delete com.github.albanread.RISCOSQEA72
```

```bash
rm -rf ~/Library/Logs/RISCOSQEA72 ~/Library/Application\ Support/RISCOSQEA72 ~/Library/Application\ Support/RISCOSQEMU
```

## Licences

- **RISC OS** is copyright RISC OS Developments Ltd, developed by
  [RISC OS Open Ltd](https://www.riscosopen.org), and published under the
  Apache 2.0 licence. The ROM in the app is their 5.30 Raspberry Pi
  release, with our HostFS module added.
- **The applications on the disc** belong to their authors and keep their
  own licences.
- **The emulator** is built on [QEMU](https://www.qemu.org) and is free
  software under the GNU General Public License, version 2. Its source is
  [albanread/RISCOSQEMUA72](https://github.com/albanread/RISCOSQEMUA72),
  branch `riscos-pi4`.
