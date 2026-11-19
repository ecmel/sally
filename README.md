# Sally

An Atari 800 XL emulator for macOS, iPhone and iPad, written in
Objective-C and C with no dependencies beyond the system frameworks
(Cocoa or UIKit, Metal, Core Audio, GameController). It is named after the XL's CPU, the 6502C "Sally".

The goals are accuracy where games can tell the difference, and frames
that reach the screen evenly: scrolling should be as smooth as on the real
machine.

## Building and running

```sh
make                          # build/Sally.app
make run ROM=game.xex         # or drop an XEX, disk, tape or cartridge onto the window
make test                     # CPU, XEX, disk and tape tests (downloads Klaus Dormann's test binary)
make tools                    # build/sallyrun, a headless runner
make install                  # copy to /Applications (INSTALL_DIR=... for elsewhere)
```

`make notarize` builds `build/Sally.dmg`, signed with
`$APPLE_SIGNING_IDENTITY` and notarized with `$APPLE_ID`,
`$APPLE_PASSWORD` and `$APPLE_TEAM_ID`. Add `ARCHS="arm64 x86_64"` for a
universal app and `VERSION=1.2.3` to stamp the version. Pushing a `v*`
tag runs the same build on GitHub Actions
(`.github/workflows/release.yml`) and attaches the image to a release.
Releases are built for Apple silicon only.

Requires macOS 14 and the Xcode command line tools. Cartridges are plain
8K or 16K images (`.rom`, `.bin`) or `.car` files of those types, or of
the bank-switched MegaCart (16K to 1M) and SIC! (128K to 512K). Sally also
runs standalone Atari executables (`.xex`). Reset and Power Cycle reload
the executable from the beginning. The last file opened comes back at
the next launch.

Disk images (`.atr`, single, enhanced or double density) go into four
drives, D1 to D4. Opening one puts it in D1 and boots it. Opening another
while a disk is in D1 swaps it in without a reset, for the next disk or
side of a game when it asks; Power Cycle then boots from it. Opening
several disks at once (in the Open panel, or dropped together) fills D1
onwards in the order of their names and boots D1. Opening a cartridge or
XEX ejects the disks. Reset boots D1 again. Writes go straight back to
the image file, unless the image's header marks it write protected (bit
0 of byte 15, as APE sets it) or the file cannot be written.

ATX images (`.atx`) keep a disk as it was recorded, copy protection
included: each track's sectors with their place on the track, the
controller's errors (CRC errors, lost data, missing sectors), duplicate
sectors and weak bits. They are read as the disk turns, at 288 rpm with
the head stepping between tracks, so a read takes as long as on the
drive and which copy of a duplicate sector comes back depends on when it
is asked for; the drive's status shows the controller's last status.
They load as fast as a real drive, much slower than ATR, and are write
protected.

Cassette images (`.cas`) go into the recorder. Opening one boots it, as
power on with START held does: the OS loads the boot records, and the
program can read more through SIOV, open the next file on the tape as
`C:` through CIO, or play the tape into POKEY's serial input by running
the motor, with the gaps and baud rates of the image.
Opening another while a tape is in swaps it in, rewound, without a reset.
Power Cycle and Reset boot the tape again from the start. Tapes are read
only, and the turbo formats (`fsk` and `pwm` chunks) are not supported.

While a drive or the recorder is at work, its busy light shows in the
top right corner of the picture: a red dot for each drive, D1 rightmost,
and one for the recorder left of them.

The machine has 128K of RAM: the 800 XL's 64K and 64K more, banked into
`$4000-$7FFF` through PORTB as on the 130XE, with separate CPU and ANTIC
access. Games that need 128K, like the Prince of Persia port, run.

`make install` registers the installed app with Launch Services, so Finder
opens `.rom`, `.car`, `.xex`, `.atr`, `.atx` and `.cas` files in Sally and shows them with a document
icon. `.bin` files list Sally under Open With, without making it their
default.

## iPhone and iPad

The iOS app (iOS 17 or later, landscape only) is built with Xcode from a
project that [XcodeGen](https://github.com/yonaskolb/XcodeGen) generates
from `project.yml` into `build/ios` (`brew install xcodegen`). It builds
Release by default (`CONFIGURATION=Debug` for a debug build), since the
emulator needs the optimizer to keep up.

```sh
make ios-sim                                  # build, install and launch in the simulator
make ios-sim SIMULATOR="iPad (9th generation)"
make ios-device DEVICE="My iPhone"            # build, install and launch on a device
make ios-open                                 # open the project in Xcode
```

`xcrun devicectl list devices` lists device names. The device must be
registered with the development team in `project.yml`.

Cartridges, XEX files, disks and tapes live in Sally's folder in the Files app.
Copy them there, open one from Files or another app, or use Import File in
the menu (the button in the top left corner), which also lists them all
and can import several disks at once. Disks behave as on the Mac, and
their writes go to the copy in Sally's folder. The last file opened comes
back at the next launch.

On screen, a touch anywhere on the left half of the screen holds fire,
and a touch anywhere on the right half steers the joystick toward it
from the drawn stick. START, SELECT and OPTION are in a row at the
bottom center and stay down while touched. Reset, power cycle and pause are in
the menu. A game controller works as on the Mac, with Menu as START and
Options as SELECT, and hides the on-screen stick and fire button while it
is connected. A hardware keyboard has the Mac's keys (Command with 1 to 4
for the console keys and HELP). The machine pauses while the app is in
the background.

## Operating system

Sally runs a small built-in OS (`src/os.c`) in place of the Atari OS ROM:
the reset sequence, interrupt dispatch through the OS vectors, the vertical
blank with its shadow registers and timers, and a stand-in character set.
That covers cartridges and XEX programs that drive the hardware themselves.
With a disk in D1 it boots from it as the Atari OS does, and DSKINV and
SIOV reach the drives, which the machine answers directly. With START
held at power on it boots from the cassette, through SIOV too, and CIO
opens the cassette as `C:` for reading, also answered by the machine;
CIO has no other devices. Programs with
their own serial routines reach the same drives over an emulated serial
bus: command frames through POKEY's serial output under the command line,
replies through serial input and its interrupt. Boot disks of games work,
copy protected ones too as ATX images; Atari DOS does not, since there is
no screen editor or disk handler. Some XEX programs require DOS or
additional OS services.

## Controls

| Mac                     | Atari                                     |
|-------------------------|-------------------------------------------|
| Arrow keys              | Joystick                                  |
| Option                  | Fire                                      |
| Cmd-1 / Cmd-2 / Cmd-3   | START / SELECT / OPTION (held)            |
| Cmd-4                   | HELP (held)                               |
| Cmd-5 / Cmd-6           | RESET / power cycle                       |
| The key left of 1       | The Inverse key                           |
| Forward Delete (fn-Del) | BREAK                                     |
| Other keys              | The Atari keyboard, by character          |
| Game controller         | Joystick (A, B or R2 fire)                |

The 800 XL's metal keys are on Command and the digits, in the order they
sit on the machine; its plastic keys are on plain Mac keys. START, SELECT,
OPTION and HELP stay down while their shortcuts are held, so a game or the
OS can see OPTION held at boot. Cmd-P pauses, Cmd-0 fits the window to the
picture, Ctrl-Cmd-F enters full screen.

## Control socket

Other programs can drive Sally through a socket: everything a player can
do, and the picture as it is drawn. Start it with `-control ADDRESS`,
where the address is a port number (TCP, listening on 127.0.0.1 only) or
the path of a Unix socket:

```sh
build/Sally.app/Contents/MacOS/Sally -control 6502 game.rom
make run ROM=game.rom CONTROL=/tmp/sally.sock
```

The machine keeps running when its window is hidden or minimized or the
display sleeps: without display refreshes, a timer paces the frames by
the clock and keeps the socket answering.

With `-headless` Sally opens no window, plays no sound and shows no
Dock icon: only the socket drives it, and it exits with the program that
started it (it ignores Ctrl-C, which is meant for that program). It answers each command as it arrives, so a program can pause
the machine and advance it with `step` as fast as the Mac allows, with
several copies running side by side:

```sh
build/Sally.app/Contents/MacOS/Sally -headless -control /tmp/sally1.sock game.rom
```

Commands are lines of text. Each one is answered with `ok`, `error
MESSAGE` or the reply listed below. Inputs from the socket add to the
keyboard and controllers rather than replacing them, and stay held until
changed. As with the keyboard, a press shorter than a frame still lasts
two frames.

| Command             | Does                                                         |
|---------------------|--------------------------------------------------------------|
| `stick UDLR` / `-`  | Joystick directions held, e.g. `stick UL`; `-` centers it    |
| `fire 0` / `1`      | Fire button released or held                                 |
| `consol SEO` / `-`  | START, SELECT, OPTION held, e.g. `consol S`                  |
| `key HH`            | Presses and releases an Atari key (keyboard code in hex, bit 6 Shift, bit 7 Control; HELP is `11`) |
| `keydown HH`        | Holds an Atari key down                                      |
| `keyup HH`          | Releases it                                                  |
| `shift 0` / `1`     | SHIFT held on its own                                        |
| `break`             | BREAK                                                        |
| `reset`             | RESET                                                        |
| `power`             | Power cycle                                                  |
| `pause 0` / `1`     | Pauses or resumes                                            |
| `load PATH...`      | Opens a cartridge, XEX, disk or tape, or several disks for D1 onwards, as opening files does (relative paths use Sally's working directory; a path with spaces among several goes in double quotes) |
| `eject`             | Removes the current cartridge, XEX or tape and all disks     |
| `status`            | Replies `status frame N paused P cart C disks DDDD`, a digit for each of D1-D4 (1 when it holds a disk) |
| `step N`            | Runs N frames (decimal) at once, paused or not, then replies `ok` |
| `peek ADDR [N]`     | Replies `peek ADDR` and N bytes (hex, default 1, at most 100) of memory as the CPU sees it; I/O reads `FF` |
| `quit`              | Replies `ok` and exits                                       |
| `frame`             | Replies with the latest frame (see below)                    |
| `stream 0` / `1`    | Sends every frame from now on, or stops                      |
| `palette`           | Replies `palette 768`, then 256 RGB triples                  |

A frame is GTIA's output for lines 8-247 of the beam, the 192 color
clocks of each that reach the screen. It comes as a line `frame N 240
LENGTH`, then LENGTH bytes: for each of the 240 lines, a flag byte and
then

- flag 0: 192 bytes, one per color clock;
- flag 1: 384 bytes, one per half color clock, on a line drawn in high
  resolution (ANTIC modes 2, 3 and F), where the two halves of a color
  clock can differ in luminance.

Each byte is an Atari color value, which `palette` maps to RGB. N counts
frames emulated since launch. Most games have no high resolution lines,
which makes a frame 46,320 bytes. A stream sends each frame as the beam
finishes it, about 60 a second (none while paused). Streamed frames
arrive between replies, so a client reads the first word of each line to
tell them apart. A client that falls behind misses frames instead of
slowing the emulator down.

```python
import socket
s = socket.create_connection(("127.0.0.1", 6502))
f = s.makefile("rb")
s.sendall(b"consol S\n"); f.readline()       # hold START
s.sendall(b"frame\n")
_, n, h, length = f.readline().split()
data, rows, i = f.read(int(length)), [], 0
for _ in range(int(h)):
    if data[i]:                                # 384 half clocks
        rows.append(data[i + 1:i + 385]); i += 385
    else:                                      # 192 clocks, doubled to 384
        rows.append(bytes(c for c in data[i + 1:i + 193] for _ in (0, 1))); i += 193
```

## How it works

`src/` holds the machine in C (C is Objective-C's base language, and the
hot loop has no message sends) and the frontends in Objective-C: what both
share in `src/`, the Mac's in `src/mac` and iOS's in `src/ios`.

- `cpu.c`: the 6502 with every bus cycle modelled, dummy reads and writes
  included, and the undocumented opcodes. Each access first waits for a
  cycle ANTIC leaves free, so cycle stealing, WSYNC and interrupt timing
  fall out of the bus.
- `antic.c`: display lists, the per-line DMA map (display list, playfield,
  player/missile and memory refresh cycles), all character and bitmap
  modes, fine scrolling, and DLI/VBI NMIs at cycle 8.
- `gtia.c`: players and missiles, the chip's priority equations,
  collisions, GTIA modes 9-11 and the palette. A line is drawn in segments:
  before any GTIA register write, the line is drawn up to the beam, so
  colors and positions changed in the middle of a line land where they
  should.
- `pokey.c`: event-driven sound. Each channel's divider runs out at a known
  cycle; the output level between those events is integrated over each
  audio sample. Also the timers and their interrupts, keyboard, RANDOM and
  the serial port.
- `machine.c`: the XL memory map (PORTB banking of the OS, BASIC and self
  test ROMs and the 130XE's extra RAM), the PIA, cartridges with their
  bank switching, XEX loading, the disk drives and their SIO commands
  (through SIOV or the serial bus), the cassette recorder, and resets.
- `os.c` and `asm6502.c`: the built-in OS, written in 6502 assembly and
  assembled at startup by a small two-pass assembler.

The frontend:

- `Emulator.m`: the emulation thread. A `CAMetalDisplayLink` asks for each
  frame shortly before the display shows it. When the display's rate is a
  whole multiple of the Atari's (60 or 120 Hz), Atari frames lock to
  refreshes, so every frame is on screen for the same time; otherwise they follow the clock. The frame is drawn by a
  fragment shader at a whole number of physical pixels per color clock and
  per scanline, so no line is thicker than another and nothing ripples
  while scrolling. It shows 228 lines, leaving out 6 lines of overscan at
  the top and bottom where games draw nothing, so the picture fits 7 times
  into a 1600-pixel-high screen.
- `Audio.m`: an AudioUnit (RemoteIO on iOS) at the device's own rate, fed through a lock-free
  ring buffer. Locking frames to a 60 Hz display runs the machine 0.13%
  fast, so the sample rate is nudged (by at most 0.5%) to keep the buffer
  level steady instead of letting it drift into dropouts.
- `keys.c`: characters to Atari keys, for both keyboards.
- `mac/SallyView.m`, `mac/Keyboard.m`, `mac/main.m`: the window, keyboard
  mapping, menus, drag and drop, game controllers.
- `ios/GameViewController.m`, `ios/TouchControls.m`, `ios/main.m`: the
  screen, the on-screen controls, the file library and menu, game
  controllers and a hardware keyboard.
- `control.c`: the control socket. The emulation thread polls it from a
  60 Hz timer and runs its commands between frames, so the socket touches
  the machine from the same thread as everything else. When the display
  link stops calling, the same timer runs the frames.

The app icon, an arcade ball-top joystick on sky blue, is drawn in code by
`tools/makeicon.m` at build time and packed with `iconutil` (for iOS, drawn
full bleed into an asset catalog).

Set `SALLY_STATS=1` to print pacing figures every second: refresh rate,
frames emulated, missed refreshes, time per update and audio buffer level.

## Checking

- `make test`: NMOS opcode cycle counts, Klaus Dormann's functional test,
  XEX loading with ordered initialization, RAM banking and resets, and
  disk boot, sector reads and writes, swapping and write protection, the
  serial bus, ATX sectors read as the disk turns (duplicates, errors,
  missing and weak sectors), and cassette boot, reads and playback.
- `build/sallyrun [-l LABELS] FILE SCRIPT`: runs a cartridge, XEX, disk or tape headless and
  saves screenshots, with the same script commands as the retro-ports
  harness (`stick`, `fire`, `consol`, `key`, `shot`), so its frames can be
  compared with that harness's reference emulator. It also helps debug a
  program: `regs`, `peek`, `poke`, breakpoints (`bp`), memory watches
  (`watch`), a profile of CPU cycles by routine (`prof`), a log of the
  sound registers (`pokeylog`) and a WAV of the sound (`wav`). With an ld65
  or VICE label file (`-l`), addresses can be names and are printed as
  `routine+offset`. The full list is at the top of `tools/sallyrun.c`.

## Not done yet

CIO devices other than `C:` (and DOS), turbo tapes and writing to tape, bank-switched cartridges
other than MegaCart and SIC! (and flash writes on those), paddles, a way
to load the BASIC ROM, and the finer GTIA quirks (player retriggering when
HPOS changes mid-object). Only NTSC is emulated.

## License

MIT. See [LICENSE](LICENSE).
