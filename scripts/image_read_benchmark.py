#!/usr/bin/env python3
"""Times reading one screenful of image files cold, under the I/O strategies a loader can use on slow storage.

Windows only: every variant is a Win32 I/O strategy. How to run it, and what to verify before trusting the numbers:
docs/build.md, "Image read benchmark".
"""
import argparse
import csv
import ctypes
import math
import os
import random
import shutil
import statistics
import subprocess
import sys
import threading
import time
from ctypes import wintypes

LAYOUTS = ('name-order', 'shuffled')
FILLER_NAME = 'filler.bin'
MIN_FILE_SIZE = 250 << 10
MAX_FILE_SIZE = 500 << 10
BUFFER_SIZE = 512 << 10  # MAX_FILE_SIZE rounded up to SECTOR_ALIGNMENT fits
SECTOR_ALIGNMENT = 4096  # unbuffered I/O: a multiple of the 512 and 4096 byte logical sectors in use
EVICTION_CHUNK = 1 << 20
MAX_IN_FLIGHT = 8

GENERIC_READ = 0x80000000
FILE_SHARE_READ_WRITE = 0x3
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_MORE_DATA = 234
ERROR_IO_PENDING = 997
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
INFINITE = 0xFFFFFFFF
MEM_COMMIT_RESERVE = 0x3000
PAGE_READWRITE = 0x04
IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS = 0x00560000
IOCTL_DISK_PERFORMANCE = 0x00070020
FSCTL_GET_RETRIEVAL_POINTERS = 0x00090073


class OVERLAPPED(ctypes.Structure):
    _fields_ = [('Internal', ctypes.c_size_t), ('InternalHigh', ctypes.c_size_t), ('Offset', wintypes.DWORD),
                ('OffsetHigh', wintypes.DWORD), ('hEvent', wintypes.HANDLE)]


class DISK_EXTENT(ctypes.Structure):
    _fields_ = [('DiskNumber', wintypes.DWORD), ('StartingOffset', ctypes.c_longlong), ('ExtentLength', ctypes.c_longlong)]


class VOLUME_DISK_EXTENTS(ctypes.Structure):
    # One extent: a volume spanning several disks is not supported
    _fields_ = [('NumberOfDiskExtents', wintypes.DWORD), ('Extent', DISK_EXTENT)]


class DISK_PERFORMANCE(ctypes.Structure):
    _fields_ = [('BytesRead', ctypes.c_longlong), ('BytesWritten', ctypes.c_longlong), ('ReadTime', ctypes.c_longlong),
                ('WriteTime', ctypes.c_longlong), ('IdleTime', ctypes.c_longlong), ('ReadCount', wintypes.DWORD),
                ('WriteCount', wintypes.DWORD), ('QueueDepth', wintypes.DWORD), ('SplitCount', wintypes.DWORD),
                ('QueryTime', ctypes.c_longlong), ('StorageDeviceNumber', wintypes.DWORD), ('StorageManagerName', ctypes.c_wchar * 8)]


class RETRIEVAL_POINTERS_BUFFER(ctypes.Structure):
    # The first extent only: it holds the file's first cluster
    _fields_ = [('ExtentCount', wintypes.DWORD), ('StartingVcn', ctypes.c_longlong), ('NextVcn', ctypes.c_longlong),
                ('Lcn', ctypes.c_longlong)]


kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                 wintypes.DWORD, wintypes.HANDLE]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                              ctypes.POINTER(OVERLAPPED)]
kernel32.GetFileSizeEx.restype = wintypes.BOOL
kernel32.GetFileSizeEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_longlong)]
kernel32.DeviceIoControl.restype = wintypes.BOOL
kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                                     wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.CreateIoCompletionPort.restype = wintypes.HANDLE
kernel32.CreateIoCompletionPort.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.c_size_t, wintypes.DWORD]
kernel32.GetQueuedCompletionStatus.restype = wintypes.BOOL
kernel32.GetQueuedCompletionStatus.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(ctypes.c_size_t),
                                               ctypes.POINTER(ctypes.c_void_p), wintypes.DWORD]
kernel32.VirtualAlloc.restype = ctypes.c_void_p
kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(2)


def raise_last_error(context):
    code = ctypes.get_last_error()
    raise OSError(None, f'{context}: {ctypes.FormatError(code).strip()}', None, code)


def open_handle(path, access=GENERIC_READ, flags=0):
    handle = kernel32.CreateFileW(path, access, FILE_SHARE_READ_WRITE, None, OPEN_EXISTING, flags, None)
    if handle is None or handle == INVALID_HANDLE_VALUE:
        raise_last_error(path)
    return handle


def aligned_buffer(size):
    address = kernel32.VirtualAlloc(None, size, MEM_COMMIT_RESERVE, PAGE_READWRITE)
    if not address:
        raise_last_error('VirtualAlloc')
    return address


def file_size(handle):
    size = ctypes.c_longlong()
    if not kernel32.GetFileSizeEx(handle, ctypes.byref(size)):
        raise_last_error('GetFileSizeEx')
    return size.value


def round_up_to_sector(size):
    return (size + SECTOR_ALIGNMENT - 1) // SECTOR_ALIGNMENT * SECTOR_ALIGNMENT


def request_size(size, unbuffered):
    request = round_up_to_sector(size) if unbuffered else size
    if request > BUFFER_SIZE:
        raise ValueError(f'A {size} byte file exceeds the {BUFFER_SIZE} byte read buffer')
    return request


def read_whole_file(handle, buffer, unbuffered):
    """One ReadFile of the whole file, as QFile::readAll issues for a file of known size."""
    size = file_size(handle)
    transferred = wintypes.DWORD()
    if not kernel32.ReadFile(handle, buffer, request_size(size, unbuffered), ctypes.byref(transferred), None):
        raise_last_error('ReadFile')
    if transferred.value != size:
        raise OSError(f'Short read: {transferred.value} of {size} bytes')


def device_io_control(handle, code, in_buffer, out_buffer, accepted_error=None):
    returned = wintypes.DWORD()
    in_pointer, in_size = (ctypes.byref(in_buffer), ctypes.sizeof(in_buffer)) if in_buffer is not None else (None, 0)
    if not kernel32.DeviceIoControl(handle, code, in_pointer, in_size, ctypes.byref(out_buffer), ctypes.sizeof(out_buffer),
                                    ctypes.byref(returned), None):
        if ctypes.get_last_error() != accepted_error:
            raise_last_error(f'DeviceIoControl {code:#x}')


def first_cluster(handle):
    starting_vcn = ctypes.c_longlong(0)
    pointers = RETRIEVAL_POINTERS_BUFFER()
    # ERROR_MORE_DATA only reports extents beyond the first
    device_io_control(handle, FSCTL_GET_RETRIEVAL_POINTERS, starting_vcn, pointers, accepted_error=ERROR_MORE_DATA)
    return pointers.Lcn


class DiskCounters:
    """Reads and read bytes of the physical disk holding a volume, counted by the disk driver.

    Must be closed before a remount: an open disk handle can hold up the dismount.
    """

    def __init__(self, path_on_volume):
        drive = os.path.splitdrive(os.path.abspath(path_on_volume))[0]
        volume = open_handle(rf'\\.\{drive}', access=0)
        try:
            extents = VOLUME_DISK_EXTENTS()
            device_io_control(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, None, extents)
        finally:
            kernel32.CloseHandle(volume)
        self._disk = open_handle(rf'\\.\PhysicalDrive{extents.Extent.DiskNumber}', access=0)

    def read(self):
        performance = DISK_PERFORMANCE()
        device_io_control(self._disk, IOCTL_DISK_PERFORMANCE, None, performance)
        return performance.ReadCount, performance.BytesRead

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        kernel32.CloseHandle(self._disk)


def read_serially(paths, buffers, flags=0):
    done = []
    start = time.perf_counter()
    for path in paths:
        handle = open_handle(path, flags=flags)
        try:
            read_whole_file(handle, buffers[0], unbuffered=bool(flags & FILE_FLAG_NO_BUFFERING))
        finally:
            kernel32.CloseHandle(handle)
        done.append(time.perf_counter() - start)
    return done


def read_in_cluster_order(paths, buffers):
    """Opens the whole batch first: sorting by first cluster needs every file's handle."""
    done = []
    handles = []
    start = time.perf_counter()
    try:
        for path in paths:
            handles.append(open_handle(path))
        for handle in sorted(handles, key=first_cluster):
            read_whole_file(handle, buffers[0], unbuffered=False)
            done.append(time.perf_counter() - start)
    finally:
        for handle in handles:
            kernel32.CloseHandle(handle)
    return done


def read_on_threads(paths, buffers, thread_count, flags=0):
    unbuffered = bool(flags & FILE_FLAG_NO_BUFFERING)
    pending = iter(paths)
    pending_lock = threading.Lock()
    done = []
    errors = []
    barrier = threading.Barrier(thread_count + 1)
    start = 0.0

    def work(buffer):
        barrier.wait()
        while True:
            with pending_lock:
                path = next(pending, None)
            if path is None:
                return
            try:
                handle = open_handle(path, flags=flags)
                try:
                    read_whole_file(handle, buffer, unbuffered)
                finally:
                    kernel32.CloseHandle(handle)
            except OSError as error:
                errors.append(error)
                return
            done.append(time.perf_counter() - start)

    threads = [threading.Thread(target=work, args=(buffers[i],)) for i in range(thread_count)]
    for thread in threads:
        thread.start()
    start = time.perf_counter()
    barrier.wait()
    for thread in threads:
        thread.join()
    if errors:
        raise errors[0]
    return done


def read_overlapped(paths, buffers, depth):
    """Keeps `depth` unbuffered reads in flight from this thread; a completion opens the next file in its slot."""
    port = kernel32.CreateIoCompletionPort(INVALID_HANDLE_VALUE, None, 0, 1)
    if not port:
        raise_last_error('CreateIoCompletionPort')
    overlappeds = [OVERLAPPED() for _ in range(depth)]
    handles = [None] * depth
    sizes = [0] * depth
    pending = iter(paths)
    done = []

    def issue(slot):
        path = next(pending, None)
        if path is None:
            return False
        handles[slot] = open_handle(path, flags=FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED)
        sizes[slot] = file_size(handles[slot])
        if not kernel32.CreateIoCompletionPort(handles[slot], port, slot, 0):
            raise_last_error('CreateIoCompletionPort')
        overlapped = overlappeds[slot]
        overlapped.Internal = overlapped.InternalHigh = overlapped.Offset = overlapped.OffsetHigh = 0
        if not kernel32.ReadFile(handles[slot], buffers[slot], request_size(sizes[slot], unbuffered=True), None,
                                 ctypes.byref(overlapped)):
            if ctypes.get_last_error() != ERROR_IO_PENDING:
                raise_last_error('ReadFile')
        return True

    transferred = wintypes.DWORD()
    slot = ctypes.c_size_t()
    completed = ctypes.c_void_p()
    start = time.perf_counter()
    try:
        in_flight = sum(issue(s) for s in range(depth))
        while in_flight:
            if not kernel32.GetQueuedCompletionStatus(port, ctypes.byref(transferred), ctypes.byref(slot),
                                                      ctypes.byref(completed), INFINITE):
                raise_last_error('GetQueuedCompletionStatus')
            now = time.perf_counter()
            s = slot.value
            if transferred.value != sizes[s]:
                raise OSError(f'Short read: {transferred.value} of {sizes[s]} bytes')
            kernel32.CloseHandle(handles[s])
            handles[s] = None
            done.append(now - start)
            if not issue(s):
                in_flight -= 1
    finally:
        # A read in flight at its handle's close still lands in valid memory: the buffers live until exit
        for handle in handles:
            if handle:
                kernel32.CloseHandle(handle)
        kernel32.CloseHandle(port)
    return done


VARIANTS = {
    'buffered': read_serially,
    'buffered-sorted': read_in_cluster_order,
    'buffered-t4': lambda paths, buffers: read_on_threads(paths, buffers, 4),
    'unbuffered-t4': lambda paths, buffers: read_on_threads(paths, buffers, 4, FILE_FLAG_NO_BUFFERING),
    'overlapped-q4': lambda paths, buffers: read_overlapped(paths, buffers, 4),
    'overlapped-q8': lambda paths, buffers: read_overlapped(paths, buffers, 8),
}


def evict_drive_cache(filler_path, buffer):
    """Random 1 MiB reads over the whole filler: the drive keeps recent reads in its own cache, which a remount does not clear."""
    handle = open_handle(filler_path, flags=FILE_FLAG_NO_BUFFERING)
    try:
        chunks = list(range(file_size(handle) // EVICTION_CHUNK))
        random.shuffle(chunks)
        transferred = wintypes.DWORD()
        overlapped = OVERLAPPED()
        for chunk in chunks:
            offset = chunk * EVICTION_CHUNK
            overlapped.Offset = offset & 0xFFFFFFFF
            overlapped.OffsetHigh = offset >> 32
            # A synchronous handle: the OVERLAPPED only carries the offset
            if not kernel32.ReadFile(handle, buffer, EVICTION_CHUNK, ctypes.byref(transferred), ctypes.byref(overlapped)):
                raise_last_error(filler_path)
    finally:
        kernel32.CloseHandle(handle)


def read_metadata(paths):
    """Opens each file and queries its size: reads its MFT record and none of its data."""
    for path in paths:
        handle = open_handle(path)
        try:
            file_size(handle)
        finally:
            kernel32.CloseHandle(handle)


def generate(root, file_count, filler_mib, seed):
    rng = random.Random(seed)
    sizes = [rng.randint(MIN_FILE_SIZE, MAX_FILE_SIZE) for _ in range(file_count)]
    names = [f'IMG_{i:05}.jpg' for i in range(file_count)]
    write_orders = {'name-order': list(range(file_count)), 'shuffled': rng.sample(range(file_count), file_count)}
    for layout in LAYOUTS:
        folder = os.path.join(root, layout)
        if os.path.isdir(folder):
            print(f'{folder} exists, kept')
            continue
        # Written under a temporary name: an interrupted generation leaves no folder that looks complete
        partial = folder + '.partial'
        shutil.rmtree(partial, ignore_errors=True)
        os.makedirs(partial)
        start = time.perf_counter()
        # Own random content per file: identical files could share a malware scan verdict
        for i in write_orders[layout]:
            with open(os.path.join(partial, names[i]), 'wb') as file:
                file.write(os.urandom(sizes[i]))
        os.rename(partial, folder)
        print(f'{folder}: {file_count} files, {sum(sizes) >> 20} MiB in {time.perf_counter() - start:.0f} s')

    filler = os.path.join(root, FILLER_NAME)
    if os.path.exists(filler):
        print(f'{filler} exists, kept')
        return
    chunk = 8 << 20
    with open(filler + '.partial', 'wb') as file:
        for _ in range((filler_mib << 20) // chunk):
            file.write(os.urandom(chunk))
    os.rename(filler + '.partial', filler)
    print(f'{filler}: {filler_mib} MiB')


def remount_volume(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        fail(f'The remount command failed: {" ".join(command)}\n{result.stderr}')
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        fail(f'The remount command printed no volume root: {" ".join(command)}')
    return lines[-1]


def completion_metrics(done):
    times = sorted(done)
    at_fraction = lambda fraction: times[math.ceil(len(times) * fraction) - 1] * 1000.0
    return {'first_ms': times[0] * 1000.0, 'half_ms': at_fraction(0.5), 'p95_ms': at_fraction(0.95), 'all_ms': times[-1] * 1000.0}


def print_summary(rows, variants):
    print(f'\n{"layout":<11} {"variant":<16} {"n":>3} {"all ms":>8} {"min":>8} {"cv %":>5} {"first":>7} {"half":>7} {"95%":>7}'
          f' {"reads/file":>10} {"disk/batch":>10}')
    for layout in LAYOUTS:
        for variant in variants:
            cell = [r for r in rows if r['layout'] == layout and r['variant'] == variant]
            if not cell:
                continue
            totals = [r['all_ms'] for r in cell]
            median = lambda key: statistics.median(r[key] for r in cell)
            cv = 100.0 * statistics.pstdev(totals) / statistics.fmean(totals)
            reads_per_file = statistics.fmean(r['disk_reads'] / r['files'] for r in cell)
            byte_ratio = statistics.fmean(r['disk_read_bytes'] / r['batch_bytes'] for r in cell)
            print(f'{layout:<11} {variant:<16} {len(cell):>3} {median("all_ms"):>8.1f} {min(totals):>8.1f} {cv:>5.1f}'
                  f' {median("first_ms"):>7.1f} {median("half_ms"):>7.1f} {median("p95_ms"):>7.1f} {reads_per_file:>10.2f} {byte_ratio:>10.2f}')


def run(args):
    variants = args.variants.split(',')
    unknown = [v for v in variants if v not in VARIANTS]
    if unknown:
        fail(f'Unknown variants: {", ".join(unknown)}; known: {", ".join(VARIANTS)}')
    cold = bool(args.remount)
    root = remount_volume(args.remount) if cold else args.root

    batches = {}
    for layout in LAYOUTS:
        folder = os.path.join(root, layout)
        if not os.path.isdir(folder):
            fail(f'{folder} does not exist: generate creates it')
        names = sorted(os.listdir(folder))
        batches[layout] = [names[i:i + args.batch] for i in range(0, len(names) - args.batch + 1, args.batch)]
    needed = args.samples * len(variants)
    available = min(len(b) for b in batches.values())
    if needed > available:
        fail(f'{needed} batches needed per layout, {available} available: generate more files or take fewer samples')

    seed = args.seed if args.seed is not None else time.time_ns() % 1_000_000
    rng = random.Random(seed)
    # No batch is read twice in a run; a random order keeps consecutive samples off neighbouring batches
    unread_batches = {layout: rng.sample(range(available), needed) for layout in LAYOUTS}
    buffers = [aligned_buffer(BUFFER_SIZE) for _ in range(MAX_IN_FLIGHT)]
    metadata = 'warm' if args.warm_metadata else 'cold'
    fields = ['seed', 'round', 'layout', 'variant', 'metadata', 'batch', 'files', 'batch_bytes', 'first_ms', 'half_ms', 'p95_ms',
              'all_ms', 'disk_reads', 'disk_read_bytes']
    new_file = not os.path.exists(args.out)
    if not new_file:
        with open(args.out, newline='') as existing:
            if next(csv.reader(existing), []) != fields:
                fail(f'{args.out} has other columns: write to a new file')

    print(f'{"Cold" if cold else "Warm, for testing the script only"}: {args.samples} rounds of {len(variants)} variants'
          f' x {len(LAYOUTS)} layouts, {args.batch} files per sample, metadata {metadata}, seed {seed}', flush=True)
    if cold:
        evict_drive_cache(os.path.join(root, FILLER_NAME), aligned_buffer(EVICTION_CHUNK))

    jobs = [(layout, variant) for layout in LAYOUTS for variant in variants]
    rows = []
    with open(args.out, 'a', newline='') as out:
        writer = csv.DictWriter(out, fieldnames=fields)
        if new_file:
            writer.writeheader()
        for round_index in range(args.samples):
            rng.shuffle(jobs)
            for layout, variant in jobs:
                if cold:
                    root = remount_volume(args.remount)
                folder = os.path.join(root, layout)
                os.listdir(folder)  # untimed: the loader starts after the folder is listed
                batch_index = unread_batches[layout].pop()
                paths = [os.path.join(folder, name) for name in batches[layout][batch_index]]
                if args.warm_metadata:
                    read_metadata(paths)
                with DiskCounters(folder) as disk:
                    reads_before, bytes_before = disk.read()
                    done = VARIANTS[variant](paths, buffers)
                    reads_after, bytes_after = disk.read()
                # Only after the read: a size query reads the file's metadata
                batch_bytes = sum(os.path.getsize(p) for p in paths)
                row = {'seed': seed, 'round': round_index, 'layout': layout, 'variant': variant, 'metadata': metadata, 'batch': batch_index,
                       'files': len(paths), 'batch_bytes': batch_bytes, **completion_metrics(done),
                       'disk_reads': reads_after - reads_before, 'disk_read_bytes': bytes_after - bytes_before}
                writer.writerow({k: f'{v:.2f}' if isinstance(v, float) else v for k, v in row.items()})
                out.flush()
                rows.append(row)
            print(f'Round {round_index + 1} of {args.samples} done', flush=True)
    print_summary(rows, variants)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)

    generate_parser = commands.add_parser('generate', help='Write both layouts and the drive-cache filler; existing ones are kept')
    generate_parser.add_argument('root')
    generate_parser.add_argument('--files', type=int, default=2400, help='Files per layout (default 2400)')
    generate_parser.add_argument('--filler-mib', type=int, default=512, help='Must exceed twice the drive cache (default 512)')
    generate_parser.add_argument('--seed', type=int, default=1)

    run_parser = commands.add_parser('run', help='Take samples: every round times each variant on each layout once, shuffled')
    run_parser.add_argument('root', help='Ignored with --remount, which prints the root')
    run_parser.add_argument('--samples', type=int, required=True, help='Rounds')
    run_parser.add_argument('--batch', type=int, default=40, help='Files per sample: one screenful (default 40)')
    run_parser.add_argument('--variants', default=','.join(VARIANTS))
    run_parser.add_argument('--remount', action='append', default=[],
                            help='Cold samples: the command run before every sample, one --remount per argument, the program first.'
                                 ' It must remount the volume and print its root last.')
    run_parser.add_argument('--warm-metadata', action='store_true',
                            help="Reads each batch file's metadata untimed before the sample, leaving its data cold")
    run_parser.add_argument('--seed', type=int, help='Reproduces the batch and variant order of an earlier run')
    run_parser.add_argument('--out', required=True, help='Per-sample CSV, appended to')

    args = parser.parse_args()
    if args.command == 'generate':
        generate(args.root, args.files, args.filler_mib, args.seed)
    else:
        run(args)


if __name__ == '__main__':
    main()
