# Sally

An Atari 800 XL emulator for macOS, written in Objective-C and C with no
dependencies beyond the system frameworks (Cocoa, Metal, Core Audio,
GameController). It is named after the XL's CPU, the 6502C "Sally".

The goals are accuracy where games can tell the difference, and frames
that reach the screen evenly: scrolling should be as smooth as on the real
machine.

## Building and running

```sh
make                          # build/Sally.app
make run ROM=game.rom         # or drop a cartridge onto the window
make test                     # CPU tests (downloads Klaus Dormann's test binary)
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
the bank-switched MegaCart (16K to 1M) and SIC! (128K to 512K). The last
cartridge opened comes back at the next launch.

The machine has 128K of RAM: the 800 XL's 64K and 64K more, banked into
`$4000-$7FFF` through PORTB as on the 130XE, with separate CPU and ANTIC
access. Games that need 128K, like the Prince of Persia port, run.

`make install` registers the installed app with Launch Services, so Finder
opens `.rom` and `.car` files in Sally and shows them with a Sally document
icon. `.bin` files list Sally under Open With, without making it their
default.

## Operating system

Sally runs a small built-in OS (`src/os.c`) in place of the Atari OS ROM:
the reset sequence, interrupt dispatch through the OS vectors, the vertical
blank with its shadow registers and timers, and a stand-in character set.
That covers cartridge games that drive the hardware themselves.

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

## How it works

`src/` holds the machine in C (C is Objective-C's base language, and the
hot loop has no message sends) and the Mac frontend in Objective-C.

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
  enough serial output for the OS.
- `machine.c`: the XL memory map (PORTB banking of the OS, BASIC and self
  test ROMs and the 130XE's extra RAM), the PIA, cartridges with their
  bank switching, and resets.
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
- `Audio.m`: an AudioUnit at the device's own rate, fed through a lock-free
  ring buffer. Locking frames to a 60 Hz display runs the machine 0.13%
  fast, so the sample rate is nudged (by at most 0.5%) to keep the buffer
  level steady instead of letting it drift into dropouts.
- `SallyView.m`, `Keyboard.m`, `main.m`: the window, keyboard mapping,
  menus, drag and drop, game controllers.

The app icon, an arcade ball-top joystick on sky blue, is drawn in code by
`tools/makeicon.m` at build time and packed with `iconutil`.

Set `SALLY_STATS=1` to print pacing figures every second: refresh rate,
frames emulated, missed refreshes, time per update and audio buffer level.

## Checking

- `make test`: cycle counts of all 256 opcodes against the NMOS table, and
  Klaus Dormann's functional test.
- `build/sallyrun [-l LABELS] ROM SCRIPT`: runs a cartridge headless and
  saves screenshots, with the same script commands as the retro-ports
  harness (`stick`, `fire`, `consol`, `key`, `shot`), so its frames can be
  compared with that harness's reference emulator. It also helps debug a
  cartridge: `regs`, `peek`, `poke`, breakpoints (`bp`), memory watches
  (`watch`), a profile of CPU cycles by routine (`prof`), a log of the
  sound registers (`pokeylog`) and a WAV of the sound (`wav`). With an ld65
  or VICE label file (`-l`), addresses can be names and are printed as
  `routine+offset`. The full list is at the top of `tools/sallyrun.c`.

## Not done yet

Disk drives and SIO (no `.atr` or `.xex`), bank-switched cartridges
other than MegaCart and SIC! (and flash writes on those), paddles, a way
to load the BASIC ROM, and the finer GTIA quirks (player retriggering when
HPOS changes mid-object). Only NTSC is emulated.

## License

MIT. See [LICENSE](LICENSE).
