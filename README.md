# MdvDecode

Decode logic-analyzer captures of Sinclair QL / ZX Spectrum microdrives into
`.MDVRAW` cartridge images.

## What it does

Reads a raw logic trace of the two data lines coming out of a spinning
microdrive (as sampled by a logic analyzer), performs PLL recovery,
aligns the two interleaved tracks, decodes each sector, merges copies
from multiple tape revolutions to correct read errors, and writes the
result as a `.MDVRAW` file for use in emulators and other tools or devices.
Q-emuLator version 4 supports mounting and creating MDVRAW images.

## MDVRAW background

Existing microdrive image formats are limited to 512-byte sectors and don't
support alternative operating systems like GST 68K/OS. `.MDVRAW` was
designed as a lower-level tape representation that can be used by any QL-family
OS without an emulator needing to make assumptions about the tape format.

It isn't meant to replace block-based formats — those stay easier for most
tools to consume — but it makes GST 68K/OS and OPD microdrives usable in
software emulators and opens the door to preserving non-standard formats in
general or experimenting with new formats.
Hardware microdrive emulators only need to play back the byte stream; no
block-structure logic is required on their side.

The image is a byte stream (100 kbit/s for QL cartridges, 80 kbit/s for ZX
Spectrum) plus a 1-bit-per-byte "gap bitmap" that marks each byte as either
signal or a "gap" that contains no magnetic transitions.
That's a middle ground between inflexible block-based formats and unwieldy
raw flux dumps.
Some tapes could have gap durations that are not an integer number of bytes
(OPD tapes have a half-byte gap after the block header), but it is hoped that
approximating to byte multiples still works while simplifying the code to
access the images.
Note that a microdrive tape is a loop and since the QL can write anywhere on
it, it's possible for a data sector or header to wrap around.
A typical MDVRAW image is around 200 KB.
On the physical tape there are two interleaved tracks, but MDVRAW stores them
as a single non-interleaved stream.
See [MDVRAW_FORMAT.md](MDVRAW_FORMAT.md) for the full spec.

### Why use a logic analyzer?

While it might be possible to capture the microdrive stream of data and gap
information from the ZX8302, using a logic analyzer results in a much more
accurate dump and software can be more flexible than the strict PLL and timings
of the ZX8302, potentially allowing to better recover tapes that are stretched,
have misaligned tracks, or exhibit intermittent errors that would stump the ZX8302.
Inexpensive USB logic analyzers work well; a microdrive breakout cable or clip
probes onto the microdrive port are enough for physical connection and even allow
dumping ZX microdrives from a QL (the ZX ULA uses a different bitrate).
Dumps are also too long to fit in the memory of an unexpanded QL.

### The preamble ambiguity

The pattern that marks a block preamble on the tape (six or more zero
bytes followed by `FF FF`) can also appear inside sector data. On a real
QL, timing tells them apart — the OS knows when it just requested a read
versus when it's mid-sector.
Emulators like Q-emuLator preserve that timing and let the guest OS handle
the format the way it would on hardware.
For emulators that don't have a cycle-accurate CPU emulation, it may be
possible to read most MDVRAWs by assuming a fixed number of cycles per
instruction. Try values around 16-18 cycles per instruction.
Standalone tools that extract sectors from a `.MDVRAW` without
an emulated CPU can use heuristics: most real preambles sit close
to a preceding gap, and the header's `recognizedFileSystem` field can
help disambiguate.

## Capturing a trace

See [LogicTrace.md](LogicTrace.md).

## Building

Visual Studio 2022, x64 Release.
The decoder itself is pure C++, but GDI+ is used for the optional diagnostic
image output, so the build is Windows-only for now. Open `MdvDecode.sln`
and build `Release | x64`; the output is `x64/Release/MdvDecode.exe`.

## Usage

```
MdvDecode [options] <input> [<output_file>]
```

`<input>` is either a PulseView `.sr` (or plain `.zip`) capture file or a
directory containing already-extracted `logic-1-1`, `logic-1-2`, ... chunks.

Options:

| Flag | Effect |
|---|---|
| `-verbose` / `-v` | Print per-block diagnostic output |
| `-opd` | ICL OPD cartridge (2 headers per sector) |
| `-zx` | ZX Spectrum Interface 1 cartridge (80 kHz signal rate) |
| `-channels <ch1> <ch2>` | Force logic-analyzer channels for track 1 / track 2 |
| `-freq <Hz>` | Override trace sampling frequency (default: read from .sr capture metadata, or 24000000) |
| `-jpg` | Also save a diagnostic block-layout visualization |

If `<output_file>` is omitted, `<input>.MDVRAW` is written next to the input
(example: `foo.sr` → `foo.MDVRAW`).
The `-jpg` output is written next to the `.MDVRAW` with the same base name.
Using both -jpg and -verbose also generates a second diagnostic diagram showing
intermediate block-matching results.


### Examples

```
MdvDecode captures\my_cartridge.sr  output\my_cartridge.MDVRAW
MdvDecode -verbose -jpg captures\68KOSUtil.sr
MdvDecode -opd captures\my_opd_cartridge.sr
```

Example output:
```
Read 1000000000 bytes (41.67 seconds)
Found 1217 data chunks
6.1 copies of the tape found, 6.9 seconds each
Found 100 sectors and 0 chunks of spurious data
200 good blocks out of 200
Header and sector sizes (bytes): 17, 1029
Median header gap length: 374.0 bytes
Median sector gap length: 67.5 bytes
Detected operating system: GST
0 bad sectors
Files:
  5110 IOSSMENU.PROG
  448 DUMP.PROG
  436 CLOCK.PROC
  4242 DRAW.PROG
  3534 FORMAT.PROG
  852 RENAME.PROG
  1114 SLIDES.PROG
  1744 TIME.PROG
  948 DELETE.PROG
  1722 ERRMSG.PROC
  290 COLOUR.PROG
  292 MEMMAP.PROG
  1512 COPY.PROG
  11412 EDIT.PROG
  586 PRINT.PROG
  340 SPACE.PROG
  1002 BAUD.PROG
Tape is 173060 bytes long including gaps (6.9 seconds)
All good!
```

Example of debug picture generated with the -jpg option:

![Trace Blocks Layout](blocks.jpg)

Legend for -jpg option:

![Blocks Jpg Legend](block_legend.png)

## Output

The `.MDVRAW` file format is documented in [MDVRAW_FORMAT.md](MDVRAW_FORMAT.md).

## Status

- **QL microdrives** (QDOS, GST 68K/OS): working well, file listings and
  checksums verified against multiple test cartridges.
- **ICL OPD** ("One Per Desk"): working with `-opd`.
- **ZX Spectrum (Interface 1)**: use the `-zx` option. So far only tried with
  logic traces collected on a QL. It may or may not work with traces collected
  on the Spectrum, depending on how close the motor speed is.

### Debugging bad sectors

When a sector fails to decode, or you want to understand why one revolution
disagrees with the others, these options drill into a single chunk:

| Flag | Effect |
|---|---|
| `-flux-jpg <chunk_index>` | Save flux+alignment JPGs for that chunk's phase-lock and preamble regions |
| `-flux-byte <block_byte>` | Also save a mid-block flux JPG per track around that byte of the block |
| `-flux-window <bytes>` | Half-window (per track) around `-flux-byte`, default 16 |
| `-dump-block <merged_idx>` | Print the merged block's raw copies from every rev to stdout for comparison |
| `-flux-block <merged_idx>` | With `-flux-byte`, one JPG per track stacking every revolution of that block |

Typical flow:

1. Run with `-verbose` — each "bad sector" line prints the sector's `blockId` (the
   merged-block index) and `dataSize`, followed by a summary of how the individual
   revolutions compared. That summary answers the first question worth asking:

   - *every revolution read this identically* — the bytes on the tape really are
     what was decoded, so the damage predates the capture and re-reading the
     cartridge cannot help.
   - *revolutions disagree* — information was lost on the way in, and a cleaner
     capture (better head contact, another pass) may well recover the sector.

   The same line reports how many revolutions carried flux gaps and how far the
   worst one strays from the merged result, which says how localized the damage is.
2. `-flux-block <blockId> -flux-byte <block_byte>` draws every revolution's flux
   around that byte, stacked top to bottom in one picture, and prints the byte
   each revolution read. Lanes are normalized to a common width so the bit cells
   line up even when the tape speed drifted between revolutions, which makes a
   dropout in one revolution obvious against the others.

   `<block_byte>` is an offset into the block, so for QDOS add 12 to the offset
   within the sector payload: 4 bytes of block header plus the 8-byte internal
   preamble.

Each picture shows the raw flux waveform, the decoder's bit-cell boundaries as
grey ticks, and the decoded `0`/`1` value inside each cell (LSB first within a
byte). To drill into one revolution on its own, `-dump-block <blockId>` lists
every revolution's raw bytes together with its chunk index, and
`-flux-jpg <chunk> -flux-byte <offset>` renders that single chunk, including its
phase-lock and preamble regions.

`-verbose -jpg` also writes a phase-2 diagnostic diagram alongside the main JPG,
with a red `1` or `2` overlaid on any block whose raw flux contains a gap longer
than about 2 bit-periods on track 1 or track 2 respectively — a quick way to
spot chunks that MdvDecode would be unlikely to recover no matter what since
the raw data is missing. For more explanations about this diagram, see the comment
in SaveDrawing.cpp.

## ReadExample

Example code is provided showing how to identify the location of sectors in MDVRAW
images.
ReadExample.exe can also be used to convert from .MDVRAW to .MDV format.

## Acknowledgements

Directly unzipping `.sr` capture files is powered by [miniz](https://github.com/richgel999/miniz)
by Rich Geldreich (MIT license — see `third_party/miniz/LICENSE`).

## License

GPL v3. See [LICENSE](LICENSE).
