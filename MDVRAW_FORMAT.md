# MDVRAW file format

The `.MDVRAW` format is a raw serialized microdrive tape image plus a
bitmap that marks which bytes in the stream are "real" and which are
inter-block gaps. It is the interchange format between MdvDecode and
Q-emuLator (creator IDs 1 and 2 respectively).
File format version 1.0.

## Overall layout

```
+-------------------+  offset 0
|      Header       |  68 bytes (fixed)
+-------------------+  offset = header.dataOffset
|   Data section    |  header.dataLength bytes
+-------------------+  optional 0–3 zero-byte pad to 4-byte alignment
|  Gap bitmap       |  header.gapBitmapLength bytes
+-------------------+  optional 0–3 zero-byte pad
| (optional extension) |
+-------------------+
```

Multi-byte fields are little-endian. Structs are naturally aligned; no
compiler padding is present in the header at v1.0.

## Header (68 bytes)

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0  | 8 | `id` | ASCII `"QLMDVRAW"`, no NUL terminator |
| 8  | 2 | `headerSize` | Size of this header in bytes (68 for v1.0) |
| 10 | 2 | `fileFormatMajorVersion` | 1 for this version |
| 12 | 2 | `fileFormatMinorVersion` | 0 for this version |
| 14 | 2 | `creatorId` | 1 = MdvDecode, 2 = Q-emuLator |
| 16 | 2 | `creatorMajorVersion` | Creator's version |
| 18 | 2 | `creatorMinorVersion` | Creator's version |
| 20 | 2 | `creatorRevision` | Creator's version |
| 22 | 1 | `ulaFamily` | 0 = unknown, 1 = ZX8302 (QL), 2 = Interface 1 (ZX Spectrum) |
| 23 | 1 | `recognizedFileSystem` | 0 = unknown, 1 = QDOS, 2 = Spectrum, 3 = OPD, 4 = GST/OS |
| 24 | 4 | `frequency` | Signal frequency in Hz (100000 for QL, 80000 for Spectrum) |
| 28 | 4 | `flags` | Reserved. Bit 0 = write protect. Unused bits must be 0 |
| 32 | 4 | `dataOffset` | File offset of the data section |
| 36 | 4 | `dataLength` | Length of the data section in bytes |
| 40 | 4 | `gapBitmapOffset` | File offset of the gap bitmap |
| 44 | 4 | `gapBitmapLength` | Length of the gap bitmap in bytes |
| 48 | 4 | `junctionStartOffset` | Offset into the data section of an unreliable region, or 0 if unused |
| 52 | 4 | `junctionLength` | Length of the unreliable region in bytes, or 0 if unused |
| 56 | 4 | `extensionOffset` | File offset of an optional extension block, or 0 if unused |
| 60 | 4 | `recommendedInitialTapePosition` | Byte offset into data section where sector-0 header starts, or `0xFFFFFFFF` if unknown |
| 64 | 4 | `currentTapePosition` | Optional: byte offset to resume from. Set to 0 or a valid position |

## Data section

A byte stream representing one full revolution of the tape. The stream
concatenates all sectors, inter-sector headers, preambles, and gap regions
in the physical order they appear on the tape.

Note that the tape is a loop and it's possible for a block that starts near
the end of the data section to wrap around to the beginning.

Time-to-byte conversion: `seconds = dataLength * 4 / frequency`.
For a QL cartridge at 100 kHz this yields ≈ 40 µs per byte.

Gap regions are stored as literal `0x00` bytes in the data section; the
gap bitmap distinguishes them from real (all-zeros) data.

## Gap bitmap

One bit per byte of the data section, MSB-first within each bitmap byte.

- **0** → the corresponding data byte is a gap (no signal on the tape).
- **1** → the corresponding data byte is real (for example, sector data).

`gapBitmapLength` equals `ceil(dataLength / 8)`. If `dataLength` is not a
multiple of 8, the trailing low bits of the last gap-bitmap byte are
undefined padding and should be ignored.

## Junction (optional)

A microdrive tape is a physical loop with a "junction" where the two ends
of the tape are joined. Data written across the junction is usually
unreliable. If the creator knows where the junction lies, it can flag
that region via `junctionStartOffset` and `junctionLength`.

## Extension

`extensionOffset` reserves space for a future append-only structure. It
should be 0 for v1.0 files. Future versions may define a chained
tag-length-value scheme here.

## Endianness and alignment

- All multi-byte numeric fields are little-endian (x86 native).
- Each section (data, gap bitmap) is followed by 0–3 zero bytes so that
  the following section starts at a 4-byte-aligned file offset.
  `dataLength` and `gapBitmapLength` in the header record the *logical*
  size, not including the trailing pad. Readers should trust
  `dataOffset` / `gapBitmapOffset` to locate each section rather than
  computing them from lengths.

## Compatibility

Writers must set `headerSize` to the size actually written (68 for v1.0).
Readers that encounter a larger `headerSize` should skip the extra bytes
and continue. Unknown values in `ulaFamily`, `recognizedFileSystem`, or
`flags` should be treated as "unknown / no assumption".
`recognizedFileSystem` is just a hint and may become stale if the the
image gets reformatted in a different format. If an emulator can detect
the FORMAT command, it could set this field back to unknown, or based on
the ROM type.

## Accessing MDVRAW images

The MDVRAW format is mainly meant to be accessed by real or emulated QL
systems where the OS and the ZX8302 handle reading and writing data.
To make it easier to also create tools that directly access sectors in
MDVRAW images, a ReadExample folder is provided with a code example that
reads an image and finds all sectors in it (assuming it was formatted by
QDOS). The code also converts it to a .MDV image.
