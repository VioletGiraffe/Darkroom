# Image read performance on an HDD

[← Back to build, tests, CI, and release](build.md#image-read-benchmark)

Measurements by `scripts/image_read_benchmark.py` and a queue-depth sweep made while validating its method, 2026-10-25.

## Setup

| | |
|---|---|
| Disk | Seagate ST6000VX001, 6 TB SATA HDD, Windows AHCI driver |
| Volume | 3 GiB fixed VHDX on that disk, NTFS, indexing off |
| Files | two layouts of 2400 files, 250-500 KiB each, about 0.9 GB per layout |
| Sample | one batch of 40 files after a remount; data cold, metadata cold or warmed by `--warm-metadata` |
| Cells | 5 samples each |

## Cold metadata

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

## Warm metadata

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

## Queue depth without metadata

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

## Not covered

- A folder scattered across a large span of a full disk, where longer seeks leave more to reordering.
