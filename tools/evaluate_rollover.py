#!/usr/bin/env python3
"""Evaluate rollover decisions against manually annotated, non-overlapping windows.

Ground truth CSV: window_start,window_end_exclusive,label,event_at
label is rollover or no_rollover; event_at (UTC Unix seconds) is required for rollover.
Results JSONL: lines emitted by the edge process (event=posture_result).
Do not score the model on its training annotations.
"""
import argparse
import csv
import json
import statistics
from collections import Counter
from pathlib import Path


LABELS = {"rollover", "no_rollover"}
STATUSES = {"rollover_detected", "no_rollover", "unjudgeable"}


def evaluate(truth_path, result_path):
    truth = {}
    with open(truth_path, encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            bed = row.get("bed_id", "")
            start = int(row["window_start"])
            end = int(row["window_end_exclusive"])
            label = row["label"]
            if label not in LABELS or end <= start:
                raise ValueError("invalid truth label or window")
            event = int(row["event_at"]) if row.get("event_at") else None
            if (label == "rollover") != (event is not None):
                raise ValueError("rollover must specify event_at, no_rollover must not")
            if event is not None and not start <= event < end:
                raise ValueError("event_at outside its window")
            key = (bed, start, end)
            if key in truth:
                raise ValueError("duplicate ground truth window")
            truth[key] = (label, event)
    predictions = {}
    with open(result_path, encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("event") != "posture_result":
                continue
            key = (
                record.get("bed_id", ""),
                record["window_start"],
                record["window_end_exclusive"],
            )
            if key not in truth:
                continue
            if key in predictions:
                raise ValueError("duplicate result for annotated window")
            status = record["result"]["status"]
            if status not in STATUSES:
                raise ValueError("invalid prediction")
            predictions[key] = (status, record.get("evaluated_at"))
    counts = Counter()
    latencies = []
    for key, (label, event_at) in truth.items():
        status, evaluated = predictions.get(key, ("unjudgeable", None))
        if status == "unjudgeable":
            counts["unjudgeable"] += 1
            counts["unjudgeable_positive" if label == "rollover" else "unjudgeable_negative"] += 1
        elif label == "rollover":
            counts["true_positive" if status == "rollover_detected" else "false_negative"] += 1
            if status == "rollover_detected" and evaluated is not None:
                latencies.append(max(0, int(evaluated) - event_at))
        else:
            counts["false_positive" if status == "rollover_detected" else "true_negative"] += 1
    positives = sum(v[0] == "rollover" for v in truth.values())
    negatives = len(truth) - positives
    tp, fp, tn, fn = (counts[x] for x in ("true_positive", "false_positive", "true_negative", "false_negative"))
    def ratio(numerator, denominator):
        return numerator / denominator if denominator else None
    return {
        "windows": len(truth),
        "positive_windows": positives,
        "negative_windows": negatives,
        "counts": dict(counts),
        "recall_conservative": ratio(tp, positives),
        "false_positive_rate_conservative": ratio(fp, negatives),
        "precision_decided": ratio(tp, tp + fp),
        "recall_decided": ratio(tp, tp + fn),
        "unjudgeable_rate": ratio(counts["unjudgeable"], len(truth)),
        "mean_detection_latency_seconds": statistics.mean(latencies) if latencies else None,
        "median_detection_latency_seconds": statistics.median(latencies) if latencies else None,
        "latency_samples": len(latencies),
        "note": "Detection latency includes the fixed 180s window schedule; absent results count as unjudgeable.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("truth_csv", type=Path)
    parser.add_argument("edge_log_jsonl", type=Path)
    args = parser.parse_args()
    print(json.dumps(evaluate(args.truth_csv, args.edge_log_jsonl), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
