#!/usr/bin/env python3
"""Cross-validation for the coverage plugin (offline acceptance tool).

Supports multi-wave runs: wave staging dirs are processed in order and the
cumulative coverage C is carried across waves, mirroring the plugin.

Verifies:
  1. Point-set parsing: plugin-stored points vs an independent cobertura parse
     of the kept XML reports (per file).
  2. Greedy selection with cross-wave carry: reference greedy must satisfy
     final cumulative == union of all files across all waves, and the
     plugin's selected.txt must reach the same union.
  3. Exact minimum set cover per wave by brute force (small N only).

Usage:
  python validate_coverage.py --store-dir <run_dir> --reports-dir <export_dir>
      --selected-file <run_dir/selected.txt> [--brute-force]
"""

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET


def fnv1a32(text):
    h = 2166136261
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def fnv1a64(text):
    h = 14695981039346656037
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def make_point(path, line):
    return (fnv1a32(path) << 32) | line


def canonical_path(filename, sources):
    if len(filename) >= 2 and filename[1] == ":":
        return filename
    for root in sources:
        candidate = root + "\\" + filename
        if os.path.exists(candidate):
            return candidate
    return sources[0] + "\\" + filename if sources else filename


def parse_cobertura(xml_path):
    """Independent reference parser producing the same point-key set."""
    tree = ET.parse(xml_path)
    root = tree.getroot()
    sources = [s.text or "" for s in root.findall("sources/source")]
    points = set()
    for pkg in root.findall("packages/package"):
        for cls in pkg.findall("classes/class"):
            filename = cls.get("filename")
            if not filename:
                continue
            path = canonical_path(filename, sources)
            for line in cls.findall("lines/line"):
                if int(line.get("hits", 0)) > 0:
                    number = int(line.get("number", 0))
                    if number > 0:
                        points.add(make_point(path, number))
    return points


def greedy_select(file_points, cumulative):
    """Reference greedy: repeatedly pick the file adding the most NEW points."""
    selected = []
    remaining = dict(file_points)
    while True:
        best = None
        best_new = 0
        for name, pts in remaining.items():
            new_count = sum(1 for p in pts if p not in cumulative)
            if new_count > best_new:
                best_new = new_count
                best = name
        if best is None or best_new == 0:
            break
        selected.append(best)
        cumulative |= remaining[best]
        del remaining[best]
    return selected


def brute_force_min_cover(file_points, target):
    """Exact minimum set cover reaching target coverage (small N only)."""
    names = list(file_points.keys())
    n = len(names)
    best_size = n + 1
    for mask in range(1, 1 << n):
        size = bin(mask).count("1")
        if size >= best_size:
            continue
        union = set()
        for i in range(n):
            if mask & (1 << i):
                union |= file_points[names[i]]
        if union >= target:
            best_size = size
    return best_size


def load_wave(wave_dir, reports_dir):
    """Returns (plugin_sets, ref_sets, file_results) for one wave."""
    plugin_sets = {}
    ref_sets = {}
    results = []
    for name in sorted(os.listdir(wave_dir)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(wave_dir, name), encoding="utf-8") as f:
            data = json.load(f)
        file_name = data["file"]
        plugin_points = set(data["points"])
        plugin_sets[file_name] = plugin_points

        xml_path = os.path.join(reports_dir, name[:-5] + ".xml")
        if not os.path.exists(xml_path):
            results.append((file_name, plugin_points, None, False, "missing xml"))
            continue
        ref_points = parse_cobertura(xml_path)
        ref_sets[file_name] = ref_points
        ok = plugin_points == ref_points
        diff = len(plugin_points ^ ref_points)
        results.append((file_name, plugin_points, ref_points, ok, diff))
    return plugin_sets, ref_sets, results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store-dir", required=True,
                        help="run dir containing wave-* staging dirs")
    parser.add_argument("--reports-dir", required=True,
                        help="dir with kept cobertura XMLs")
    parser.add_argument("--selected-file", required=True,
                        help="plugin selected.txt path")
    parser.add_argument("--brute-force", action="store_true",
                        help="run exact minimum set cover per wave")
    args = parser.parse_args()

    waves = sorted(
        d for d in os.listdir(args.store_dir) if d.startswith("wave-"))
    if not waves:
        print(f"[FAIL] no wave-* dirs under {args.store_dir}")
        return 1

    failed = 0
    cumulative = set()
    reference_selected = []
    union_all = set()

    print(f"== multi-wave validation: {len(waves)} waves ==")
    for wave in waves:
        wave_dir = os.path.join(args.store_dir, wave)
        plugin_sets, ref_sets, results = load_wave(wave_dir, args.reports_dir)

        print(f"--- {wave} ---")
        for file_name, plugin_points, ref_points, ok, extra in results:
            if ref_points is None:
                print(f"[FAIL] {file_name}: {extra}")
                failed += 1
                continue
            print(f"[{'PASS' if ok else 'FAIL'}] {file_name}: "
                  f"plugin={len(plugin_points)} ref={len(ref_points)} xor={extra}")
            if not ok:
                failed += 1

        wave_all = set()
        for pts in plugin_sets.values():
            wave_all |= pts
        union_all |= wave_all

        cumulative_before = set(cumulative)
        selected_wave = greedy_select(plugin_sets, cumulative)
        wave_ok = cumulative == (cumulative_before | wave_all)
        print(f"[{'PASS' if wave_ok else 'FAIL'}] {wave}: "
              f"cumulative after wave = {len(cumulative)} "
              f"(prev {len(cumulative_before)} | wave-all {len(wave_all)})")
        if not wave_ok:
            failed += 1
        reference_selected.extend(selected_wave)

        if args.brute_force and len(plugin_sets) <= 16:
            # Only this wave's NEW points are reachable by this wave's files.
            needed = {p for p in wave_all if p not in cumulative_before}
            optimal = brute_force_min_cover(plugin_sets, needed)
            print(f"[INFO] {wave}: greedy={len(selected_wave)} "
                  f"exact-optimal={optimal}")

    final_ok = cumulative == union_all
    print(f"[{'PASS' if final_ok else 'FAIL'}] final cumulative == union(all): "
          f"{len(cumulative)} vs {len(union_all)}")
    if not final_ok:
        failed += 1

    selected_txt = []
    if os.path.exists(args.selected_file):
        with open(args.selected_file, encoding="utf-8") as f:
            selected_txt = [line.strip() for line in f if line.strip()]
    plugin_union = set()
    plugin_selected_names = set(selected_txt)
    for wave in waves:
        wave_dir = os.path.join(args.store_dir, wave)
        for name in os.listdir(wave_dir):
            if not name.endswith(".json"):
                continue
            with open(os.path.join(wave_dir, name), encoding="utf-8") as f:
                data = json.load(f)
            if data["file"] in plugin_selected_names:
                plugin_union |= set(data["points"])
    sel_ok = plugin_union == union_all
    print(f"[{'PASS' if sel_ok else 'FAIL'}] plugin selected.txt union == "
          f"union(all): {len(plugin_union)} vs {len(union_all)}")
    if not sel_ok:
        failed += 1

    if set(selected_txt) == set(reference_selected):
        print(f"[PASS] selection identical: {len(selected_txt)} files")
    else:
        print(f"[INFO] selection differs by tie-break: plugin={len(selected_txt)} "
              f"reference={len(reference_selected)} (union already verified)")

    print()
    if failed == 0:
        print("RESULT: ALL PASS")
        return 0
    print(f"RESULT: {failed} FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
