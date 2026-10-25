# Image read performance

[← Back to build, tests, CI, and release](build.md#image-read-benchmark)

Measurements by `scripts/image_read_benchmark.py` and a queue-depth sweep made while validating its method, 2026-10-25,
on an i5-12600KF.

## HDD setup

| | |
|---|---|
| Disk | Seagate ST6000VX001, 6 TB SATA HDD, Windows AHCI driver |
| Volume | 3 GiB fixed VHDX on that disk, NTFS, indexing off |
| Files | two layouts of 2400 files, 250-500 KiB each, about 0.9 GB per layout |
| Sample | one batch of 40 files after a remount; data cold, metadata cold or warmed by `--warm-metadata` |
| Cells | 5 samples each |

## HDD, cold metadata

Medians in ms for the batch.

| Layout | Variant | All | First file | Half | CV % | Disk reads per file |
|---|---|---|---|---|---|---|
| shuffled | buffered | 410 | 11.6 | 214 | 6.7 | 1.99 |
| shuffled | buffered-t4 | 400 | 10.9 | 192 | 5.0 | 1.96 |
| shuffled | unbuffered-t4 | 403 | 10.1 | 203 | 7.1 | 1.98 |
| shuffled | overlapped-q4 | 411 | 24.3 | 207 | 6.4 | 1.98 |
| shuffled | overlapped-q8 | 426 | 68.1 | 274 | 1.9 | 1.97 |
| shuffled | buffered-sorted | 442 | 39.8 | 231 | 10.1 | 1.96 |
| name-order | buffered | 103 | 12.2 | 56 | 2.6 | 1.27 |
| name-order | buffered-sorted | 104 | 13.7 | 58 | 5.0 | 1.27 |
| name-order | overlapped-q4 | 103 | 15.5 | 60 | 4.5 | 1.27 |
| name-order | overlapped-q8 | 104 | 22.3 | 68 | 6.0 | 1.27 |
| name-order | buffered-t4 | 115 | 10.3 | 68 | 10.4 | 1.98 |
| name-order | unbuffered-t4 | 122 | 18.4 | 75 | 5.4 | 1.97 |

- No variant reads a scattered batch faster than serial buffered reads beyond the noise: the best is 2-3% faster at
  a CV of 5-7%.
- 8 reads in flight delay the first file sixfold and half the batch by 28%: the variant opens 8 files, a metadata read
  each, before it collects the first completion.
- Sorting by first cluster costs 8%: every file is opened before the first read.
- A contiguous batch reads at about 150 MB/s under every single-threaded variant.
- Threads slow a contiguous batch by 12-19%: concurrent opens read each file's MFT record separately, where serial
  opens read them in clusters.
- A buffered `ReadFile` of a whole file is one disk read.
- Each file of a scattered batch costs one metadata read before its data read.

## HDD, warm metadata

Medians in ms for the batch; min and max over the 5 samples.

| Layout | Variant | All | Min | Max | First file | Half | CV % |
|---|---|---|---|---|---|---|---|
| shuffled | buffered | 419 | 404 | 427 | 7.5 | 223 | 1.9 |
| shuffled | buffered-t4 | 382 | 330 | 412 | 7.4 | 183 | 7.5 |
| shuffled | unbuffered-t4 | 377 | 350 | 420 | 12.8 | 196 | 6.4 |
| shuffled | overlapped-q4 | 370 | 356 | 459 | 9.2 | 201 | 9.8 |
| shuffled | overlapped-q8 | 368 | 325 | 389 | 11.6 | 199 | 5.8 |
| shuffled | buffered-sorted | 401 | 374 | 430 | 11.9 | 202 | 5.4 |
| name-order | buffered | 98 | 94 | 110 | 9.7 | 53 | 5.6 |
| name-order | other variants | 96-105 | 92-97 | 106-125 | 7.0-13.1 | 53-60 | 5.1-9.8 |

- Every file costs one disk read, its data read; the disk's read bytes match the batch's.
- On a scattered batch, threaded and queued reads are 9-12% faster than serial buffered reads. At 8 in flight the
  samples do not overlap: 325-389 ms against 404-427.
- Serial buffered reads gain nothing from warm metadata: 419 ms against 410 cold.
- A contiguous batch reads at the sequential rate under every variant.

## HDD queue depth without metadata

Random unbuffered reads at a fixed queue depth over one 4 GiB file on the same disk, outside the VHDX. IOPS:

| Block | QD1 | QD2 | QD4 | QD8 | QD16 | QD32 |
|---|---|---|---|---|---|---|
| 4 KiB | 68 | 76 | 102 | 133 | 165 | 193 |
| 400 KiB | 59 | 59 | 74 | 86 | 85 | 85 |

- At 400 KiB the gain stops at QD8, +45%; the 95th-percentile latency grows from 26 ms at QD1 to 281 ms at QD8.
- The batch measurements show a fraction of this gain, and only with warm metadata: 12% at 8 in flight. The 0.9 GB
  span keeps seeks short.
- With cold metadata none of it shows: every data read follows its file's metadata read.

## Method validation

- The VHDX layer passes queued requests through: at 8 in flight inside the VHDX, the host disk's read queue measured
  7.94, and its read rate matched the VHDX disk's.
- Reads inside the VHDX and on its backing file read raw ran at the same rate: 4 KiB at QD1, 139 vs 138 IOPS.
- The span sets absolute times: 4 KiB random reads at QD1 ran at 68 IOPS over 4 GiB and 138 over 1 GiB.

## SSD read throughput by thread count

The `threads` subcommand's method: 400 files of 391 KB on average, data cold, metadata warm, medians of 3 rounds.
Buffered reads; unbuffered reads follow the same curves.

| Drive | Interface |
|---|---|
| H: WD PC SN720 512 GB | NVMe, PCIe 3.0 x4 |
| G: Lexar ARES 2 TB | NVMe, a PCIe 4.0 drive linked at PCIe 3.0 x4 |
| F: Samsung 850 EVO mSATA 1 TB | SATA |

All 16 logical CPUs, GB/s:

| Threads | 1 | 2 | 4 | 6 | 8 | 12 | 16 |
|---|---|---|---|---|---|---|---|
| H: | 1.24 | 2.12 | 3.14 | 3.30 | 3.35 | 3.36 | 3.33 |
| G: | 1.56 | 2.81 | 3.43 | 3.52 | 3.52 | 3.39 | 3.49 |
| F: | 0.39 | 0.52 | 0.54 | 0.54 | 0.54 | 0.54 | 0.54 |

Confined to fewer CPUs, GB/s. Two cores: logical CPUs 0 and 2, two P-cores. One core: CPUs 0 and 1, the
hyperthreads of one P-core. One CPU: CPU 0.

| Threads | H: two cores | H: one core | H: one CPU | G: two cores | G: one core | G: one CPU |
|---|---|---|---|---|---|---|
| 1 | 1.29 | 1.19 | 1.18 | 1.56 | 1.51 | 1.53 |
| 2 | 2.14 | 2.07 | 2.02 | 2.83 | 2.52 | 2.64 |
| 4 | 3.17 | 2.99 | 3.00 | 3.36 | 3.26 | 3.29 |
| 6 | 3.25 | 3.13 | 3.20 | 3.40 | 3.16 | 3.39 |
| 8 | 3.31 | 3.22 | 3.10 | 3.45 | 3.32 | 3.42 |

- Every drive saturates at its interface: about 3.4 GB/s for both NVMe drives, 0.54 GB/s for SATA.
- Above 4 threads the gain is at most 7%; above 6, at most 2%.
- Confined to one CPU, 6 threads come within 4% of all 16 CPUs.
- The whole pool used about 0.1-0.2 of one core at full NVMe speed, whatever the thread count: process CPU time over
  0.25 s passes, counted in 15.6 ms ticks.
- A pool sized by core count would give a one- or two-core machine 2 workers: 17-37% below 6 workers on the same CPUs,
  on the NVMe drives.

## Not covered

- HDD: a folder scattered across a large span of a full disk, where longer seeks leave more to reordering.
- SSD: cold metadata, and PCIe 4.0 links.
- SSD: a small CPU busy decoding. The benchmark cannot show it: under load from other processes, its Python threads
  serialize on the interpreter lock.
