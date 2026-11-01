#!/usr/bin/env python3
"""Fits the tone curve of LibRaw renderings (ToneCurve in Shared/src/ImageDecoding/LibRawDecoder.cpp) to cameras' own JPEGs,
and reports how closely the current and the fitted curve match them.

The target is the JPEG preview each raw embeds: the camera's own rendering. The curve maps LibRaw's linear output, after the
exposure the camera's rendering adds (cameraRenderingExposureEv), to the preview's tones:
  f(x) = max(0, (s(c(x)) - b) / (1 - b))
  c(x) = (g(x*e + toe) - g(toe)) / (g(e + toe) - g(toe)),  g(v) = v^p / (v^p + h^p)
  s(y) = y up to k, above it k + (1 - k) * (t - a*t^2*(1 - t)),  t = (y - k) / (1 - k)
  x - LibRaw's linear output, 1 at raw white; e - 2^exposureEv; k - shoulderStart, 0.4
  p, h, toe, a, b - the constants exponent, halfInput, toeOffset, shoulder, blackOffset
The curve goes through each channel, as cameras apply theirs, so fitting it per channel matches their saturation too.

The fit is of the curve's shape: each raw gets the exposure shift that fits it best, and every brightness bin weighs the same.
The app shifts each raw's exposure to match its preview's midtones anyway (previewMatchingShiftEv).
The fitted curve is then shifted so that the median raw needs none: the curve alone renders a raw without a usable preview.
blackOffset stays out of the fit, at 0: it is a choice of look, darker than the cameras' median, which the fit would undo.

Running it:
  1. Write the data with the hidden test [tonecurvedata]: it renders every camera raw at the top level of the folder named by
     DARKROOM_RAW_SAMPLES into the folder named by DARKROOM_TONE_CURVE_DATA. With both variables set:
       scripts\\run_tests.bat "[tonecurvedata]"
  2. python scripts/fit_tone_curve.py <data folder> [--exclude PREFIX ...]
     Needs numpy and Pillow. Reads the current constants from LibRawDecoder.cpp. Takes about a minute.
  3. To adopt the fit, copy the printed constants line into ToneCurve.

Exclude a raw, by the start of its file name, when its preview is not a plain global rendering of its scene: a mostly clipped
scene, or a phone's preview with local tone mapping. Monochrome raws and raws without a preview are skipped. In the raw corpus
docs/image-decode-performance.md uses, that is: --exclude 12 27

The output:
  - The fitted constants, as the line ToneCurve declares them, with the current blackOffset.
  - Residuals by brightness: the curve minus the cameras' median, in EV, per bin of LibRaw's output, each raw at its best
    shift. Near 0 across the range is a good fit, except for blackOffset's darkening in the lowest bins. The cameras disagree
    among themselves by about 1 EV in the shadows, much less above.
  - Per raw, for the current and the fitted curve: the camera JPEG's midtone exposure (EV, positive when the camera's is
    brighter), contrast and saturation, each relative to our rendering: 0, 1.00 and 1.00 match. A midtone gap the curve leaves
    comes from a camera setting the raw's metadata does not record, or from the camera's metering.
  - Per raw, the app: the current curve with the preview-matching shift the decoder chose. Its midtone gap should be near 0.

Pixels where the raw clips are left out; where the preview clips, they count, as medians allow. A preview counts as sRGB unless
the raw's EXIF declares Adobe RGB.
"""
import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

DECODER_SOURCE = Path(__file__).resolve().parent.parent / 'Shared' / 'src' / 'ImageDecoding' / 'LibRawDecoder.cpp'
CONSTANTS_PATTERN = re.compile(
    r'constexpr double exponent = ([\d.]+), halfInput = ([\d.]+), toeOffset = ([\d.]+), shoulder = ([\d.]+), blackOffset = ([\d.]+);')
SHOULDER_START = 0.4  # ToneCurve::shoulderStart
COMPARED_WIDTH = 240  # both images are box-averaged to this width before the comparison
LOG2_BINS = np.arange(-9.0, 0.01, 0.5)  # brightness bins of LibRaw's output after the exposure, in EV below raw white
MIN_BIN_PIXELS = 30
MIN_BIN_RAWS = 4  # a bin fewer raws reach is left out of the fit
LUMINANCE = np.array([0.2126, 0.7152, 0.0722])
ADOBE_RGB_TO_LINEAR_SRGB = np.array([[1.39836, -0.39836, 0.0], [0.0, 1.0, 0.0], [0.0, -0.04293, 1.04293]])
FIT_SHIFTS_EV = np.arange(-2.5, 2.501, 0.02)
# Bounds of exponent, halfInput, toeOffset and shoulder; shoulder stays under 3, where s(y) stops being monotonic
FIT_LOWER, FIT_UPPER = np.array([1.0, 0.05, 0.0, 0.0]), np.array([3.0, 0.6, 0.02, 2.9])


def read_netpbm(path):
    """A 16-bit binary PPM or PGM as the test writes it, with no comments: (height, width, channels), 0 to 1."""
    magic, size, _, pixels = path.read_bytes().split(b'\n', 3)
    width, height = map(int, size.split())
    channels = 3 if magic == b'P6' else 1
    return np.frombuffer(pixels, dtype='>u2').reshape(height, width, channels) / 65535.0


def box_resized(pixels, width, height):
    return np.stack([np.asarray(Image.fromarray(pixels[..., c].astype(np.float32), 'F').resize((width, height), Image.BOX))
                     for c in range(pixels.shape[2])], -1).astype(np.float64)


def linear_preview(path, adobe_rgb):
    codes = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64) / 255
    if adobe_rgb:
        return np.clip((codes ** 2.19921875) @ ADOBE_RGB_TO_LINEAR_SRGB.T, 0, None)
    return np.where(codes <= 0.04045, codes / 12.92, ((codes + 0.055) / 1.055) ** 2.4)


def curve(x, exponent, half_input, toe_offset, shoulder, black_offset, gain):
    """Linear output, 0 to 1, of the curve for linear input x; numpy-broadcast over every argument."""
    def g(v):
        powered = v ** exponent
        return powered / (powered + half_input ** exponent)
    toned = (g(x * gain + toe_offset) - g(toe_offset)) / (g(gain + toe_offset) - g(toe_offset))
    t = np.clip((toned - SHOULDER_START) / (1 - SHOULDER_START), 0, 1)
    toned = np.where(toned > SHOULDER_START, SHOULDER_START + (1 - SHOULDER_START) * (t - shoulder * t * t * (1 - t)), toned)
    return np.maximum((toned - black_offset) / (1 - black_offset), 0)


def load_samples(folder, excluded_prefixes):
    """Per usable raw: name, gain, and the (pixels, 3) linear pairs of LibRaw output and preview where the raw does not clip."""
    samples = []
    for entry in json.loads((folder / 'samples.json').read_text(encoding='utf-8')):
        name = entry['raw']
        if any(name.startswith(prefix) for prefix in excluded_prefixes) or not entry['preview']:
            continue
        rendering_path = folder / (name + '.ppm')
        if not rendering_path.exists():  # monochrome: .pgm
            continue

        rendering = read_netpbm(rendering_path)
        preview = Image.open(folder / (name + '.jpg'))
        if abs((rendering.shape[1] / rendering.shape[0]) / (preview.width / preview.height) - 1) > 0.03:
            print(f'skipped, aspect ratios differ: {name}', file=sys.stderr)
            continue

        height = round(COMPARED_WIDTH * preview.height / preview.width)
        x = box_resized(rendering, COMPARED_WIDTH, height)
        y = box_resized(linear_preview(folder / (name + '.jpg'), entry['previewAdobeRgb']), COMPARED_WIDTH, height)
        # The camera's clipped pixels stay: dropping them would bias its medians low in the brightest bins
        usable = (x.max(-1) < 0.99) & (x.min(-1) > 1e-5) & (y.min(-1) > 1e-5)
        samples.append({'name': name, 'exposure_ev': entry['cameraExposureEv'], 'gain': 2.0 ** entry['cameraExposureEv'],
                        'preview_shift_ev': entry['previewShiftEv'], 'x': x[usable], 'y': y[usable]})
    return samples


def binned_points(samples):
    """Per raw and brightness bin: the median input, the median log2 preview value, the raw's gain, the bin's centre, the raw's
    index in samples."""
    points = []
    for index, sample in enumerate(samples):
        x, y = sample['x'].ravel(), sample['y'].ravel()
        log2_exposed = np.log2(x * sample['gain'])
        for low, high in zip(LOG2_BINS[:-1], LOG2_BINS[1:]):
            in_bin = (log2_exposed >= low) & (log2_exposed < high)
            if in_bin.sum() > MIN_BIN_PIXELS:
                points.append((np.median(x[in_bin]), np.median(np.log2(y[in_bin])), sample['gain'], (low + high) / 2, index))
    return np.array(points)


def shifted_residuals(points, constants):
    """Per binned point, the curve minus the camera in log2, each raw at the shift with its least mean absolute residual; and
    each raw's shift."""
    x, log2_y, gain, raw = points[:, 0], points[:, 1], points[:, 2], points[:, 4]
    modelled = np.log2(np.maximum(curve(x[None, :], *constants, gain[None, :] * 2.0 ** FIT_SHIFTS_EV[:, None]), 1e-12)) - log2_y[None, :]
    residuals, shifts = np.empty(len(x)), []
    for index in np.unique(raw):
        mine = raw == index
        best = int(np.argmin(np.mean(np.abs(modelled[:, mine]), axis=1)))
        residuals[mine] = modelled[best, mine]
        shifts.append(FIT_SHIFTS_EV[best])
    return residuals, np.array(shifts)


def fitted_bins(points):
    return [centre for centre in np.unique(points[:, 3]) if (points[:, 3] == centre).sum() >= MIN_BIN_RAWS]


def nelder_mead(f, start, steps, iterations=600):
    """A local minimum of f near start: the downhill simplex, its first vertices start + each step along its axis."""
    simplex = [np.array(start, float)] + [np.array(start, float) + np.eye(len(start))[i] * step for i, step in enumerate(steps)]
    values = [f(vertex) for vertex in simplex]
    for _ in range(iterations):
        order = np.argsort(values)
        simplex, values = [simplex[i] for i in order], [values[i] for i in order]
        if values[-1] - values[0] < 1e-6:
            break
        centroid = np.mean(simplex[:-1], axis=0)
        reflected = 2 * centroid - simplex[-1]
        reflected_value = f(reflected)
        if reflected_value < values[0]:
            expanded = 3 * centroid - 2 * simplex[-1]
            expanded_value = f(expanded)
            simplex[-1], values[-1] = (expanded, expanded_value) if expanded_value < reflected_value else (reflected, reflected_value)
        elif reflected_value < values[-2]:
            simplex[-1], values[-1] = reflected, reflected_value
        else:
            contracted = (centroid + simplex[-1]) / 2
            contracted_value = f(contracted)
            if contracted_value < values[-1]:
                simplex[-1], values[-1] = contracted, contracted_value
            else:
                simplex = [(simplex[0] + vertex) / 2 for vertex in simplex]
                values = [f(vertex) for vertex in simplex]
    return simplex[int(np.argmin(values))]


def fit_shape(points, start):
    """exponent, halfInput, toeOffset and shoulder with the least mean absolute residual per bin, blackOffset 0, then leveled:
    the median raw's shift is 0."""
    bins = fitted_bins(points)

    def objective(shape):
        residuals, _ = shifted_residuals(points, (*np.clip(shape, FIT_LOWER, FIT_UPPER), 0.0))
        return np.mean([np.mean(np.abs(residuals[points[:, 3] == centre])) for centre in bins])

    exponent, half_input, toe_offset, shoulder = np.clip(nelder_mead(objective, start, (0.15, 0.03, 0.003, 0.5)), FIT_LOWER, FIT_UPPER)
    _, shifts = shifted_residuals(points, (exponent, half_input, toe_offset, shoulder, 0.0))
    # Scaling the input by 2^m equals scaling halfInput and toeOffset by 2^-m
    level = 2.0 ** -np.median(shifts)
    return round(float(exponent), 4), round(float(half_input * level), 4), round(float(toe_offset * level), 5), round(float(shoulder), 4)


def oklab(rgb):
    lms = np.cbrt(np.clip(rgb, 0, None) @ np.array([[0.4122214708, 0.5363325363, 0.0514459929], [0.2119034982, 0.6806995451, 0.1073969566],
                                                     [0.0883024619, 0.2817188376, 0.6299787005]]).T)
    return lms @ np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099],
                           [0.0259040371, 0.7827717662, -0.8086757660]]).T


def compared(ours, camera):
    """The camera's midtone exposure difference (EV), midtone contrast and saturation, relative to ours."""
    ours_y, camera_y = ours @ LUMINANCE, camera @ LUMINANCE
    midtones = (camera_y > 0.05) & (camera_y < 0.4)
    exposure = np.median(np.log2(camera_y[midtones] / np.maximum(ours_y[midtones], 1e-6)))
    # contrast: the slope of log camera luminance over log ours, through medians of five camera luminance quantile bands
    log_ours, log_camera = np.log2(np.maximum(ours_y, 1e-6)), np.log2(camera_y)
    edges = np.percentile(log_camera, [20, 35, 50, 65, 80])
    bands = [(log_camera >= low) & (log_camera < high) for low, high in zip(edges[:-1], edges[1:])]
    contrast = np.polyfit([np.median(log_ours[band]) for band in bands], [np.median(log_camera[band]) for band in bands], 1)[0]
    # saturation: Oklab chroma over lightness, on colored pixels that are not dark
    ours_lab, camera_lab = oklab(ours), oklab(camera)
    ours_chroma, camera_chroma = np.hypot(ours_lab[:, 1], ours_lab[:, 2]), np.hypot(camera_lab[:, 1], camera_lab[:, 2])
    colored = (camera_chroma > 0.02) & (camera_lab[:, 0] > 0.25) & (ours_lab[:, 0] > 0.05)
    saturation = (np.median((camera_chroma[colored] / camera_lab[colored, 0]) / (ours_chroma[colored] / ours_lab[colored, 0]))
                  if colored.sum() > 50 else np.nan)
    return exposure, contrast, saturation


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('data_folder', type=Path, help='the folder the [tonecurvedata] test wrote')
    parser.add_argument('--exclude', nargs='*', default=[], metavar='PREFIX', help='raws whose file names start with these')
    args = parser.parse_args()
    sys.stdout.reconfigure(errors='backslashreplace')  # raw file names may not fit the console's code page

    match = CONSTANTS_PATTERN.search(DECODER_SOURCE.read_text(encoding='utf-8'))
    if not match:
        sys.exit(f'The constants line is not in {DECODER_SOURCE}: update CONSTANTS_PATTERN')
    current = tuple(float(value) for value in match.groups())

    samples = load_samples(args.data_folder, args.exclude)
    if not samples:
        sys.exit('No usable raws')
    points = binned_points(samples)
    fitted = (*fit_shape(points, current[:4]), current[4])
    curves = {'current': current, 'fitted': fitted}

    print(f'{len(samples)} raws: {", ".join(sample["name"].split(" ")[0] for sample in samples)}')
    print(f'current: exponent {current[0]}, halfInput {current[1]}, toeOffset {current[2]}, shoulder {current[3]}, blackOffset {current[4]}')
    print(f'fitted:  constexpr double exponent = {fitted[0]}, halfInput = {fitted[1]}, toeOffset = {fitted[2]}, shoulder = {fitted[3]}, '
          f'blackOffset = {fitted[4]};')

    print('\nResidual by brightness, curve minus the cameras\' median (EV), each raw at its best shift')
    print('log2 of input after exposure | raws | current | fitted')
    residuals = [shifted_residuals(points, constants)[0] for constants in curves.values()]
    for centre in fitted_bins(points):
        in_bin = points[:, 3] == centre
        print(f'{centre:+6.2f} | {in_bin.sum():2d} | {np.median(residuals[0][in_bin]):+.2f} | {np.median(residuals[1][in_bin]):+.2f}')

    print('\nCamera JPEG relative to ours: midtone exposure (EV), contrast, saturation')
    print('raw | cameraExposureEv | current | fitted | previewShiftEv | the app: current with that shift')
    results = {label: [] for label in (*curves, 'app')}
    for sample in samples:
        row = []
        renderings = [(label, curve(sample['x'], *constants, sample['gain'])) for label, constants in curves.items()]
        renderings.append(('app', curve(sample['x'], *current, sample['gain'] * 2.0 ** sample['preview_shift_ev'])))
        for label, rendering in renderings:
            result = compared(rendering, sample['y'])
            results[label].append(result)
            row.append(f'{result[0]:+.2f} {result[1]:.2f} {result[2]:.2f}')
        row.insert(2, f'{sample["preview_shift_ev"]:+.2f}')
        print(f'{sample["name"][:40]:40} | {sample["exposure_ev"]:+.2f} | {" | ".join(row)}')
    for label, values in results.items():
        values = np.array(values)
        print(f'median, {label}: exposure {np.nanmedian(values[:, 0]):+.2f} EV (absolute {np.nanmedian(np.abs(values[:, 0])):.2f}), '
              f'contrast {np.nanmedian(values[:, 1]):.2f}, saturation {np.nanmedian(values[:, 2]):.2f}')


if __name__ == '__main__':
    main()
