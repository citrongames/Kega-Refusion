"""Compare the same title track against a consumer hardware recording.

Estimate one offset and clock ratio from common musical landmarks. Use the
same matched windows, lag, polarity and band-level calibration for both cores.
Metrics describe agreement with this capture, not overall chip accuracy.
The waveform score is diagnostic only: recording-chain phase and lossy
encoding can reverse its ordering even for a known reference.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import numpy as np


RATE = 16000
MAX_SECONDS = 600
HOP = 320
HZ = RATE / HOP
FFT = 1024
EDGES = np.geomspace(70, 7100, 18)
BINS = np.rint(EDGES * FFT / RATE).astype(int)
WINDOW = np.hanning(FFT)


def decode(source, output, raw):
    input_args = ["-f", "s16le", "-ar", "44100", "-ac", "2"] if raw else []
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
                    *input_args, "-i", str(source), "-t", str(MAX_SECONDS), "-map", "0:a:0", "-ac", "1",
                    "-ar", str(RATE), "-f", "f32le", str(output)], check=True)
    return np.memmap(output, dtype="<f4", mode="r")


def features(audio):
    frames = (len(audio) - FFT) // HOP + 1
    view = np.lib.stride_tricks.sliding_window_view(audio, FFT)[::HOP]
    assert len(view) == frames
    levels = np.empty((frames, len(BINS) - 1))
    rms = np.empty(frames)
    for start in range(0, frames, 384):
        block = view[start:start + 384]
        power = np.abs(np.fft.rfft(block * WINDOW)) ** 2
        for band, (lo, hi) in enumerate(zip(BINS[:-1], BINS[1:])):
            levels[start:start + len(block), band] = (
                10 * np.log10(np.maximum(power[:, lo:hi].mean(axis=1), 1e-8))
            )
        rms[start:start + len(block)] = np.sqrt(np.mean(block ** 2, axis=1))
    delta = np.diff(levels, axis=0, prepend=levels[:1])
    flux = np.clip(delta, 0, 18)
    # Combining differences across frequency bands suppresses static analog EQ.
    match = np.clip((flux - flux.mean(axis=0)) / np.maximum(flux.std(axis=0), 1), -4, 4)
    return levels, rms, flux.mean(axis=1), match


def lag_scores(pattern, query):
    size = len(pattern)
    pattern = pattern - pattern.mean(axis=0)
    pad = 1 << (size + len(query) - 2).bit_length()
    product = np.fft.rfft(query, n=pad, axis=0) * np.fft.rfft(pattern[::-1], n=pad, axis=0)
    dot = np.fft.irfft(product.sum(axis=1), n=pad)[size - 1:len(query)]
    sums = np.vstack((np.zeros((1, query.shape[1])), np.cumsum(query, axis=0)))
    squares = np.r_[0.0, np.cumsum(np.square(query).sum(axis=1))]
    variance = (squares[size:] - squares[:-size]
                - np.square(sums[size:] - sums[:-size]).sum(axis=1) / size)
    scale = np.sqrt(np.maximum(variance, 0) * np.square(pattern).sum())
    return np.divide(dot, scale, out=np.zeros_like(dot), where=scale > 1e-12)


def landmarks(pattern, hardware):
    length = int(HZ * 8)
    candidates = []
    for start in range(int(HZ * 5), len(pattern) - length, int(HZ * 16)):
        scores = lag_scores(pattern[start:start + length], hardware)
        options = []
        for _ in range(5):
            index = int(scores.argmax())
            if scores[index] < .25:
                break
            options.append((index, float(scores[index])))
            radius = int(HZ * 2.5)
            scores[max(0, index - radius):index + radius + 1] = -1
        candidates.append((start, options))
    return candidates


def fit_clock(candidates, offset_range):
    def matching(intercept, ratio):
        matches = []
        for x, options in candidates:
            target = intercept + ratio * x
            nearest = min(options, key=lambda pair: abs(pair[0] - target), default=None)
            if nearest and abs(nearest[0] - target) < HZ * .24:
                matches.append((x, nearest[0], nearest[1]))
        return tuple(matches)

    models = {}
    for i, (left, first) in enumerate(candidates):
        for right, second in candidates[i + 1:]:
            if right - left < 40 * HZ:
                continue
            for y0, _ in first:
                for y1, _ in second:
                    ratio = (y1 - y0) / (right - left)
                    if not .98 <= ratio <= 1.02:
                        continue
                    offset = y0 - ratio * left
                    if offset_range and not offset_range[0] <= offset / HZ <= offset_range[1]:
                        continue
                    matches = matching(offset, ratio)
                    if len(matches) < 5:
                        continue
                    models[matches] = (len(matches), sum(p[2] for p in matches))
    ranked = sorted(models, key=models.get, reverse=True)
    if not ranked:
        raise ValueError("fewer than five consistent landmarks in a narrow clock-ratio range")
    winner = ranked[0]
    x, y, score = np.asarray(winner).T
    clock, intercept = np.polyfit(x, y, 1)
    for alternative in ranked[1:]:
        if len(alternative) < len(winner) - 1:
            break
        alt_x, alt_y, _ = np.asarray(alternative).T
        alt_ratio, alt_intercept = np.polyfit(alt_x, alt_y, 1)
        if abs((alt_ratio - clock) * x.mean() + alt_intercept - intercept) > HZ:
            raise ValueError(
                "multiple ambiguous clock offsets match this repeating soundtrack; "
                f"offsets {intercept / HZ:.3f}s ({len(winner)} anchors) and "
                f"{alt_intercept / HZ:.3f}s ({len(alternative)} anchors)"
            )
    residual = (y - (clock * x + intercept)) / HZ
    return intercept / HZ, clock, [
        {"core_seconds": float(a / HZ), "hardware_seconds": float(b / HZ),
         "correlation": float(c), "residual_ms": float(1000 * d)}
        for a, b, c, d in zip(x, y, score, residual)
    ]


def interpolate(array, positions):
    low = np.floor(positions).astype(int)
    fraction = positions - low
    if array.ndim > 1:
        fraction = fraction[:, None]
    return array[low] * (1 - fraction) + array[low + 1] * fraction


def correlation(a, b):
    a = a - a.mean()
    b = b - b.mean()
    norm = np.sqrt(np.sum(a ** 2) * np.sum(b ** 2))
    return float(np.sum(a * b) / norm) if norm > 1e-12 else None


def compare(waves, extracted, intercept, ratio):
    old, current, hardware = waves
    old_db, old_rms, old_onset, old_match = extracted[0]
    new_db, new_rms, new_onset, new_match = extracted[1]
    hw_db, hw_rms, hw_onset, hw_match = extracted[2]
    common_match = (old_match + new_match) / 2
    min_count = min(len(old_db), len(new_db))
    points = np.arange(min_count)
    target = points * ratio + intercept * HZ
    valid = (target >= HZ) & (target + HZ < len(hw_db)) & ((old_rms[:min_count] + new_rms[:min_count]) > .002)
    training = points[valid & ((points // int(HZ * 5)) % 2 == 0)]
    if len(training) < HZ * 30:
        raise ValueError("insufficient audible overlap for common band calibration")
    mean_db = (old_db[training] + new_db[training]) / 2
    hardware_db = interpolate(hw_db, target[training])
    band_gain = np.median(hardware_db - mean_db, axis=0)
    audible_gain = float(np.median(20 * np.log10(
        np.maximum(interpolate(hw_rms, target[training]), 1e-5)
        / np.maximum((old_rms[training] + new_rms[training]) / 2, 1e-5))))
    windows = []
    length = int(HZ * 3)
    for start in range(int(HZ * 5), min_count - length, int(HZ * 10)):
        stop = start + length
        if not all(valid[start:stop]):
            continue
        points = np.arange(start, stop)
        # Common per-window refinement compensates residual clock or capture jitter.
        margin = 10
        first = int(round(target[start])) - margin
        query = hw_match[first:first + length + 2 * margin]
        matches = lag_scores(common_match[start:stop], query)
        best_match = int(matches.argmax())
        if matches[best_match] < .25 or best_match in (0, len(matches) - 1):
            continue
        band_shift = first + best_match - target[start]
        hardware_frames = target[points] + band_shift
        hw_power = interpolate(hw_db, hardware_frames)
        hw_envelope = 20 * np.log10(np.maximum(interpolate(hw_rms, hardware_frames), 1e-5))
        hw_flux = interpolate(hw_onset, hardware_frames)

        # Raw waveform is diagnostic only. Apply the same delay to both cores.
        wave_start = start * HOP + FFT // 2
        wave_length = int(RATE * 0.35)
        sample_indices = np.arange(wave_start, wave_start + wave_length)
        common_wave = (old[sample_indices] + current[sample_indices]) / 2
        wave_margin = int(RATE * .055)
        query_steps = np.arange(-wave_margin, wave_length + wave_margin)
        base = (intercept + band_shift / HZ) * RATE + wave_start * ratio
        hw_query = interpolate(hardware, base + ratio * query_steps)
        wave_scores = lag_scores(common_wave[:, None], hw_query[:, None])
        best = int(np.abs(wave_scores).argmax())
        hw_wave = hw_query[best:best + wave_length]
        common_corr = float(wave_scores[best])
        row = {"core_start_seconds": start / HZ,
               "hardware_start_seconds": float(hardware_frames[0] / HZ),
               "landmark_correlation": float(matches[best_match]),
               "landmark_refinement_ms": float(1000 * band_shift / HZ),
               "wave_refinement_ms": float((best - wave_margin) * 1000 / RATE),
               "shared_wave_polarity_probe": common_corr}
        for name, waveform, db, rms, onset in (
            ("old", old, old_db, old_rms, old_onset),
            ("current", current, new_db, new_rms, new_onset),
        ):
            envelope = 20 * np.log10(np.maximum(rms[points], 1e-5)) + audible_gain
            row[name] = {
                "band_error_db": float(np.median(np.abs(db[points] + band_gain - hw_power))),
                "level_error_db": float(np.median(np.abs(envelope - hw_envelope))),
                "level_correlation": correlation(envelope, hw_envelope),
                "onset_correlation": correlation(onset[points], hw_flux),
                "wave_correlation_raw": correlation(waveform[sample_indices], hw_wave),
            }
        windows.append(row)
    if len(windows) < 5:
        raise ValueError("fewer than five comparison windows with secure overlap")
    strong = [row["shared_wave_polarity_probe"] for row in windows
              if abs(row["shared_wave_polarity_probe"]) >= .3]
    polarity = int(np.sign(np.median(strong))) if strong else 0
    fields = ("band_error_db", "level_error_db", "level_correlation", "onset_correlation")
    if polarity:
        for row in windows:
            for name in ("old", "current"):
                raw_corr = row[name]["wave_correlation_raw"]
                row[name]["wave_correlation"] = polarity * raw_corr if raw_corr is not None else None
        fields = (*fields, "wave_correlation")
    summary = {}
    for field in fields:
        pairs = [(row["old"][field], row["current"][field], row["core_start_seconds"])
                 for row in windows if row["old"][field] is not None and row["current"][field] is not None]
        if not pairs:
            continue
        old_values, new_values, times = np.array(pairs).T
        differences = new_values - old_values
        improvement = -differences if field.endswith("_db") else differences
        summary[field] = {
            "old_median": float(np.median(old_values)), "current_median": float(np.median(new_values)),
            "paired_current_minus_old_median": float(np.median(differences)),
            "paired_p10": float(np.quantile(differences, .1)), "paired_p90": float(np.quantile(differences, .9)),
            "closer_windows": {"old": int((improvement < -1e-6).sum()),
                               "current": int((improvement > 1e-6).sum()),
                               "tied": int((np.abs(improvement) <= 1e-6).sum())},
        }
        block_id = times.astype(int) // 60
        blocks = [differences[block_id == block] for block in np.unique(block_id)]
        if len(blocks) >= 5:
            rng = np.random.default_rng(20261122)
            draws = rng.integers(len(blocks), size=(2000, len(blocks)))
            medians = [np.median(np.concatenate([blocks[index] for index in batch])) for batch in draws]
            summary[field]["descriptive_block_bootstrap_95pct"] = np.quantile(medians, [.025, .975]).tolist()
    return {"window_count": len(windows), "window_seconds": length / HZ,
            "measured_feature_seconds": len(windows) * length / HZ,
            "wave_seconds_per_window": wave_length / RATE,
            "common_gain_db": audible_gain, "common_band_gain_db": band_gain.tolist(),
            "bands_hz": EDGES.tolist(), "summary": summary,
            "waveform_diagnostic": {
                "usable_for_fidelity_ranking": False,
                "global_wave_polarity": polarity,
                "correlation": summary.pop("wave_correlation", None),
            },
            "windows": windows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("old", "current", "reference", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--offset-range", nargs=2, type=float, metavar=("MIN", "MAX"),
                        help="Optional seconds bounds from an independent start-time cue")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="matched-", dir=args.output.resolve().parent) as cache:
        waves = [decode(source, Path(cache) / (name + ".f32"), name != "reference")
                 for name, source in (("old", args.old), ("current", args.current), ("reference", args.reference))]
        triples = [features(wave) for wave in waves]
        size = min(len(triples[0][3]), len(triples[1][3]))
        shared = (triples[0][3][:size] + triples[1][3][:size]) / 2
        try:
            offset, clock, anchors = fit_clock(landmarks(shared, triples[2][3]), args.offset_range)
            report = {"method": "Mono, 16 kHz; one affine clock from eight-second spectral-flux landmarks; identical alignment and calibration for both cores",
                      "lengths_seconds": [len(wave) / RATE for wave in waves],
                      "searched_offset_range_seconds": args.offset_range,
                      "hardware_offset_seconds": offset, "hardware_seconds_per_core_second": clock,
                      "clock_drift_ppm": (clock - 1) * 1e6,
                      "landmarks": anchors}
            report.update(compare(waves, triples, offset, clock))
        except ValueError as error:
            report = {"status": "comparison unavailable", "reason": str(error),
                      "lengths_seconds": [len(wave) / RATE for wave in waves]}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k not in ("windows", "landmarks")}, indent=2))


if __name__ == "__main__":
    main()
