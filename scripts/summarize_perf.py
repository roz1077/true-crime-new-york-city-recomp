"""Summarize XBE_PERF logs without treating one-second samples as per-frame percentiles."""
import argparse
import json
from pathlib import Path
import re
import statistics


def summarize(path, warm_after, until=None):
    text = path.read_text(errors='replace')
    samples = []
    for line in text.splitlines():
        match = re.search(r'\[\s*([\d.]+)\] perf: ([\d.]+) fps ([\d.]+) ms/frame '
                          r'\| host ([\d.]+).*?compile ([\d.]+)', line)
        if match:
            samples.append(list(map(float, match.groups())))
    warm = [sample for sample in samples if sample[0] >= warm_after and (until is None or sample[0] <= until)]
    def median_metric(pattern):
        values = re.findall(pattern, text)
        selected = [float(value) for time, value in values
                    if float(time) >= warm_after and (until is None or float(time) <= until)]
        return statistics.median(selected) if selected else None
    counts = re.findall(r'\| (\d+) shaders (\d+) psos(?: (\d+) shader cache hits)? this second', text)
    return {
        'path': str(path),
        'samples': len(samples),
        'last_sample_seconds': samples[-1][0] if samples else None,
        'compiled_shaders': sum(int(row[0]) for row in counts),
        'disk_shader_hits': sum(int(row[2] or 0) for row in counts),
        'created_psos': sum(int(row[1]) for row in counts),
        'worst_compile_ms_per_frame_sample': max((row[4] for row in samples), default=None),
        'steady_median_fps': statistics.median(row[1] for row in warm) if warm else None,
        'steady_median_renderer_ms': statistics.median(row[3] for row in warm) if warm else None,
        'steady_median_assembly_ms': median_metric(r'\[\s*([\d.]+)\] perf: .*?assemble ([\d.]+)'),
        'steady_median_draws': median_metric(r'\[\s*([\d.]+)\] perf: per frame ([\d.]+) draws'),
        'steady_median_pipeline_ms': median_metric(r'\[\s*([\d.]+)\] perf: pipeline creation ([\d.]+)'),
        'audio_starvation_messages': text.count('audio: output starved'),
        'note': 'One-second averages; not p95/p99 frame times. Compare only matched scene workloads.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--warm-after', type=float, default=30)
    parser.add_argument('--until', type=float, help='End of the steady comparison window (seconds).')
    args = parser.parse_args()
    print(json.dumps([summarize(path, args.warm_after, args.until) for path in args.logs], indent=2))


if __name__ == '__main__':
    main()
