# Image decode performance

[← Back to frame viewing and playback](architecture/playback.md)

Decode times on the image viewer's JPEG and camera raw paths, measured 2026-10-31. Both machines ran the same library
sources: libjpeg-turbo 3.2.0 and LibRaw 0.22.2, from vcpkg's source trees.

## Setups

| | i5-12600KF | Raspberry Pi 4 |
|---|---|---|
| CPU | process pinned to the 6 P-cores, 12 logical CPUs | 4 Cortex-A72 at 1.8 GHz, `ondemand` governor; 1.8 GB RAM |
| Build | MSVC `/O2`, vcpkg release libraries | libjpeg-turbo: Clang 22, CMake Release; LibRaw: GCC 14 `-O3` |
| LibRaw OpenMP threads | 12 | 4 |
| Throttling | Windows power throttling disabled | none occurred: `vcgencmd get_throttled` 0x0, peak 71 °C |

## Method

- Files are read into memory before the clock starts. Every cell is the mean of two passes.
- JPEG: each decode runs `decodeJpeg`'s sequence: a fresh tj3 handle, the header, the scaling factor, a newly allocated
  output buffer, `tj3Decompress8`. A cell is the median of 5-50 repetitions, single-threaded.
- Raw: one render per pass with `decodeRaw`'s parameters. On the Pi each file renders in its own process, capped at
  1.4 GB of address space.
- Pass-to-pass spread: at most 6% on the 12600KF. On the Pi, JPEG cells reach 20%: 6 of 128 cells differ by more than 8%,
  while repetitions within a pass agree to 0.3%. Pi raw renders stay within 4%, except X-Trans full renders at 13-18%.

## Samples

The samples come from a collection of test-stack images, one per kind.

- JPEG: 16 of 12,196 files. The collection is baseline 8-bit except 47 progressive files, with no CMYK, 12-bit,
  arithmetic-coded or lossless JPEGs. Labels give the subsampling, restart markers, megapixels and source.
- Raw: 20 files, one per format and decoder.

## JPEG: DCT-reduced decodes

Each scale's time as a fraction of the full decode; the 8/8 column is the full decode in ms.

i5-12600KF:

| File | 8/8 ms | 1/8 | 2/8 | 3/8 | 4/8 |
|---|---|---|---|---|---|
| 01 422 24MP camera D7100 | 75 | 0.51 | 0.60 | 0.67 | 0.71 |
| 02 422 restart 20MP camera E-M1II | 88 | 0.64 | 0.73 | 0.77 | 0.81 |
| 03 422 47MP camera S1R | 116 | 0.46 | 0.57 | 0.64 | 0.66 |
| 04 422 50MP camera 5DS | 141 | 0.46 | 0.56 | 0.63 | 0.68 |
| 05 422 8MP 4K frame | 14 | 0.31 | 0.41 | 0.52 | 0.53 |
| 06 420 45MP D850 stacked | 120 | 0.52 | 0.60 | 0.74 | 0.71 |
| 07 420 75MP Phase One Capture One | 148 | 0.41 | 0.49 | 0.68 | 0.61 |
| 08 420 12MP D90 stacked | 27 | 0.41 | 0.51 | 0.67 | 0.64 |
| 09 420 restart 15MP Photoshop | 24 | 0.28 | 0.36 | 0.60 | 0.52 |
| 10 444 24MP Capture One | 57 | 0.43 | 0.53 | 0.65 | 0.63 |
| 11 444 restart 24MP Photoshop | 97 | 0.59 | 0.70 | 0.76 | 0.78 |
| 12 444 restart 101MP Phase One Photoshop | 245 | 0.45 | 0.57 | 0.67 | 0.65 |
| 13 gray restart 12MP camera D300 | 14 | 0.57 | 0.67 | 0.73 | 0.76 |
| 14 gray 37MP Photoshop | 55 | 0.65 | 0.76 | 0.80 | 0.81 |
| 15 progressive 444 22MP GIMP | 611 | 0.95 | 0.95 | 0.96 | 0.97 |
| 16 progressive 420 3MP Lightroom | 16 | 0.79 | 0.79 | 0.86 | 0.87 |

Raspberry Pi 4:

| File | 8/8 ms | 1/8 | 2/8 | 3/8 | 4/8 |
|---|---|---|---|---|---|
| 01 422 24MP camera D7100 | 369 | 0.34 | 0.45 | 0.46 | 0.52 |
| 02 422 restart 20MP camera E-M1II | 414 | 0.43 | 0.58 | 0.60 | 0.62 |
| 03 422 47MP camera S1R | 634 | 0.29 | 0.40 | 0.43 | 0.55 |
| 04 422 50MP camera 5DS | 728 | 0.31 | 0.41 | 0.44 | 0.57 |
| 05 422 8MP 4K frame | 54 | 0.34 | 0.51 | 0.56 | 0.62 |
| 06 420 45MP D850 stacked | 667 | 0.31 | 0.39 | 0.47 | 0.54 |
| 07 420 75MP Phase One Capture One | 937 | 0.23 | 0.30 | 0.43 | 0.47 |
| 08 420 12MP D90 stacked | 152 | 0.26 | 0.34 | 0.43 | 0.41 |
| 09 420 restart 15MP Photoshop | 156 | 0.19 | 0.25 | 0.37 | 0.31 |
| 10 444 24MP Capture One | 351 | 0.28 | 0.39 | 0.42 | 0.49 |
| 11 444 restart 24MP Photoshop | 495 | 0.40 | 0.56 | 0.56 | 0.64 |
| 12 444 restart 101MP Phase One Photoshop | 1419 | 0.31 | 0.44 | 0.51 | 0.58 |
| 13 gray restart 12MP camera D300 | 54 | 0.53 | 0.71 | 0.73 | 0.81 |
| 14 gray 37MP Photoshop | 237 | 0.51 | 0.69 | 0.70 | 0.77 |
| 15 progressive 444 22MP GIMP | 2126 | 0.89 | 0.90 | 0.90 | 0.92 |
| 16 progressive 420 3MP Lightroom | 54 | 0.78 | 0.81 | 0.87 | 0.85 |

- 1/8 is the floor every scale pays: each scale entropy-decodes the whole stream. On baseline files 1/8 costs 28-65% of
  a full decode on the 12600KF and 19-53% on the Pi.
- 2/8 is never slower than 3/8: libjpeg-turbo has a SIMD 2x2 IDCT and only a scalar 3x3 one. It saves 17-40% on 4:2:0,
  0-21% on the rest.
- 4/8 against 3/8: on the 12600KF 4/8 is 4-13% faster on 4:2:0 and up to 8% slower on 4:2:2 and gray. On the Pi 4/8 is
  slower, by up to 29%, except on two 4:2:0 files.
- libjpeg-turbo decodes 4:2:0 chroma at twice the luma's IDCT size, which skips upsampling: at 3/8 a scalar 6x6 IDCT, at
  4/8 the SIMD 8x8 one. 4:2:2 chroma, subsampled on one axis, is upsampled at every scale.
- Progressive files cost 78-97% of a full decode at every scale: entropy decoding dominates.
- Above 4/8 every IDCT is scalar: 6/8 takes up to 1.6x a full decode, 7/8 up to 2.3x.

## JPEG: arithmetic coding

The 16 JPEGs, transcoded losslessly with `tj3Transform` as `jpegtran -arithmetic` does: all markers copied, progressive
files kept progressive, restart markers dropped. Every copy decodes to pixels identical to its original. The optimized
Huffman column is the same transcode with `TJXOPT_OPTIMIZE`: the share of the saving any lossless re-encode gets.
Timed on the i5-12600KF only, by the method above; pass-to-pass spread at most 7%.

Size:

| File | Original MB | Optimized Huffman | Arithmetic | Arithmetic vs optimized |
|---|---|---|---|---|
| 01 422 24MP camera D7100 | 5.05 | -13.6% | -27.0% | -15.5% |
| 02 422 restart 20MP camera E-M1II | 6.79 | -16.2% | -24.5% | -10.0% |
| 03 422 47MP camera S1R | 6.28 | -9.2% | -21.8% | -13.8% |
| 04 422 50MP camera 5DS | 7.18 | -13.1% | -22.6% | -10.9% |
| 05 422 8MP 4K frame | 0.37 | -14.1% | -31.2% | -19.9% |
| 06 420 45MP D850 stacked | 8.47 | 0.0% | -13.5% | -13.5% |
| 07 420 75MP Phase One Capture One | 6.73 | -2.2% | -14.7% | -12.7% |
| 08 420 12MP D90 stacked | 1.31 | -2.0% | -12.7% | -10.9% |
| 09 420 restart 15MP Photoshop | 0.40 | -26.8% | -38.7% | -16.3% |
| 10 444 24MP Capture One | 1.71 | 0.0% | -18.6% | -18.6% |
| 11 444 restart 24MP Photoshop | 6.75 | -9.4% | -21.9% | -13.8% |
| 12 444 restart 101MP Phase One Photoshop | 10.30 | -8.0% | -25.3% | -18.8% |
| 13 gray restart 12MP camera D300 | 0.77 | -0.6% | -12.5% | -11.9% |
| 14 gray 37MP Photoshop | 4.34 | -2.7% | -16.0% | -13.8% |
| 15 progressive 444 22MP GIMP | 20.37 | 0.0% | -4.9% | -4.9% |
| 16 progressive 420 3MP Lightroom | 0.25 | 0.0% | -7.5% | -7.5% |
| Total | 87.1 | -6.0% | -17.0% | -11.7% |

Decode, with each scale as a fraction of the arithmetic-coded full decode:

| File | Huffman 8/8 ms | Arithmetic 8/8 ms | Slowdown | 1/8 | 2/8 | 3/8 | 4/8 |
|---|---|---|---|---|---|---|---|
| 01 422 24MP camera D7100 | 75 | 427 | 5.7x | 0.92 | 0.95 | 0.94 | 0.97 |
| 02 422 restart 20MP camera E-M1II | 88 | 565 | 6.4x | 0.93 | 0.94 | 0.96 | 0.95 |
| 03 422 47MP camera S1R | 116 | 600 | 5.2x | 0.90 | 0.91 | 0.93 | 0.92 |
| 04 422 50MP camera 5DS | 141 | 651 | 4.6x | 0.91 | 0.93 | 0.93 | 0.93 |
| 05 422 8MP 4K frame | 14 | 40 | 2.8x | 0.75 | 0.78 | 0.83 | 0.82 |
| 06 420 45MP D850 stacked | 120 | 853 | 7.1x | 0.94 | 0.93 | 0.96 | 0.95 |
| 07 420 75MP Phase One Capture One | 148 | 703 | 4.7x | 0.89 | 0.89 | 0.94 | 0.92 |
| 08 420 12MP D90 stacked | 27 | 136 | 5.0x | 0.89 | 0.91 | 0.95 | 0.93 |
| 09 420 restart 15MP Photoshop | 24 | 45 | 1.9x | 0.62 | 0.65 | 0.79 | 0.74 |
| 10 444 24MP Capture One | 57 | 183 | 3.2x | 0.82 | 0.85 | 0.89 | 0.90 |
| 11 444 restart 24MP Photoshop | 97 | 595 | 6.1x | 0.95 | 0.95 | 0.96 | 0.97 |
| 12 444 restart 101MP Phase One Photoshop | 245 | 954 | 3.9x | 0.93 | 0.92 | 0.95 | 0.93 |
| 13 gray restart 12MP camera D300 | 14 | 71 | 5.1x | 0.94 | 0.94 | 0.97 | 0.96 |
| 14 gray 37MP Photoshop | 55 | 398 | 7.3x | 0.93 | 0.94 | 0.93 | 0.94 |
| 15 progressive 444 22MP GIMP | 611 | 2033 | 3.3x | 0.99 | 0.99 | 1.00 | 0.98 |
| 16 progressive 420 3MP Lightroom | 16 | 32 | 2.0x | 0.88 | 0.90 | 0.92 | 0.92 |

- Arithmetic coding saves 10-20% over optimized Huffman on baseline files, 5-8% on progressive ones.
- A full decode is 1.9-7.3x slower than the Huffman original's, 4.6-6.4x on camera JPEGs.
- A 1/8 decode costs 62-99% of the full one, mostly 89-95%: entropy decoding dominates, as in progressive files.

## Camera raw: LibRaw renders

Total time from `unpack` through `copy_mem_image`, in ms. Linear and monochrome raws (08, 18, 19, 20) have no color
filter array: their half-size rendering is full size.

Half size:

| File | Output | 12600KF ms | Pi 4 ms | Pi / 12600KF |
|---|---|---|---|---|
| 01 ARW lossy | 3984x2660 | 290 | 1278 | 4.4 |
| 02 CR2 ljpeg | 2898x1935 | 477 | 1741 | 3.6 |
| 03 CR3 lossless | 4095x2731 | 268 | 1063 | 4.0 |
| 04 NEF lossless | 4144x2760 | 786 | 2994 | 3.8 |
| 05 NEF lossy 12-bit | 3689x2462 | 622 | 2242 | 3.6 |
| 06 NEF uncompressed | 4144x2760 | 154 | 812 | 5.3 |
| 07 NEF packed 14-bit | 3032x2020 | 96 | 480 | 5.0 |
| 08 NEFX Z8 merged linear | 16512x11008 | 2382 | out of memory | |
| 09 ORF | 2620x1956 | 486 | 1501 | 3.1 |
| 10 RAF X-Trans uncompressed | 3123x2085 | 140 | 547 | 3.9 |
| 11 RAF X-Trans compressed | 3132x2088 | 262 | 1490 | 5.7 |
| 12 RAF GFX50S uncompressed | 4140x3104 | 264 | 1143 | 4.3 |
| 13 RAF GFX100 compressed | 5831x4373 | 936 | 4410 | 4.7 |
| 14 IIQ Phase One | 7102x5326 | 1636 | 7165 | 4.4 |
| 15 RW2 S1R high-res | 8384x5610 | 970 | 4988 | 5.1 |
| 16 DNG lossless bayer | 2610x1956 | 406 | 1470 | 3.6 |
| 17 DNG uncompressed bayer | 2880x1920 | 116 | 490 | 4.2 |
| 18 DNG lossless linear | 8224x5490 | 2414 | 9424 | 3.9 |
| 19 DNG deflate float linear | 3840x5760 | 852 | 3263 | 3.8 |
| 20 DNG mono | 5984x4000 | 440 | 2342 | 5.3 |

Full size:

| File | Output | 12600KF ms | Pi 4 ms | Pi / 12600KF |
|---|---|---|---|---|
| 01 ARW lossy | 7968x5320 | 1006 | 5378 | 5.3 |
| 02 CR2 ljpeg | 5796x3870 | 928 | 3840 | 4.1 |
| 03 CR3 lossless | 8191x5463 | 1037 | 5244 | 5.1 |
| 04 NEF lossless | 8288x5520 | 1567 | 7246 | 4.6 |
| 05 NEF lossy 12-bit | 7378x4924 | 1257 | 5760 | 4.6 |
| 06 NEF uncompressed | 8288x5520 | 908 | 5096 | 5.6 |
| 07 NEF packed 14-bit | 6064x4040 | 570 | 2732 | 4.8 |
| 08 NEFX Z8 merged linear | 16512x11008 | 2382 | out of memory | |
| 09 ORF | 5240x3912 | 899 | 3507 | 3.9 |
| 10 RAF X-Trans uncompressed | 6246x4170 | 2212 | 17716 | 8.0 |
| 11 RAF X-Trans compressed | 6264x4176 | 2364 | 18968 | 8.0 |
| 12 RAF GFX50S uncompressed | 8280x6208 | 1047 | 6018 | 5.7 |
| 13 RAF GFX100 compressed | 11662x8746 | 2782 | crashed at the memory cap | |
| 14 IIQ Phase One | 14204x10652 | 4186 | out of memory | |
| 15 RW2 S1R high-res | 16768x11220 | 4330 | out of memory | |
| 16 DNG lossless bayer | 5220x3912 | 822 | 3488 | 4.2 |
| 17 DNG uncompressed bayer | 5760x3840 | 565 | 2608 | 4.6 |
| 18 DNG lossless linear | 8224x5490 | 2410 | 9514 | 3.9 |
| 19 DNG deflate float linear | 3840x5760 | 828 | 3323 | 4.0 |
| 20 DNG mono | 5984x4000 | 446 | 2228 | 5.0 |

- Over the files both machines rendered, the Pi is 4.2x slower at half size and 5.4x at full size.
- Unpacking is mostly 3-5x slower on the Pi; `dcraw_process` is 4-6x slower, except X-Trans full renders at 8.2x.
- Under the Pi's 1.4 GB cap a full rendering fails from about 100 MP up, and the 182 MP linear raw fails at either size.
  Renders up to 51 MP fit.
- An OpenMP build of LibRaw reports a failed allocation as "mempool overflowed", not as insufficient memory.
