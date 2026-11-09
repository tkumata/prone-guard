"""Regression tests for evaluation denominators and missing predictions."""
import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from evaluate_rollover import evaluate


class EvaluationTests(unittest.TestCase):
    def test_counts_missing_and_uncertain_as_unjudgeable(self):
        with tempfile.TemporaryDirectory() as root:
            labels = Path(root) / "labels.csv"
            logs = Path(root) / "edge.jsonl"
            with labels.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=[
                    "bed_id", "window_start", "window_end_exclusive", "label", "event_at"
                ])
                writer.writeheader()
                writer.writerow(dict(bed_id="bed-01", window_start=1000,
                                     window_end_exclusive=1180, label="rollover", event_at=1100))
                writer.writerow(dict(bed_id="bed-01", window_start=1180,
                                     window_end_exclusive=1360, label="no_rollover", event_at=""))
                writer.writerow(dict(bed_id="bed-01", window_start=1360,
                                     window_end_exclusive=1540, label="rollover", event_at=1450))
            with logs.open("w", encoding="utf-8") as stream:
                stream.write(json.dumps(dict(event="posture_result", bed_id="bed-01",
                    window_start=1000, window_end_exclusive=1180, evaluated_at=1190,
                    result=dict(status="rollover_detected"))) + "\n")
                stream.write(json.dumps(dict(event="posture_result", bed_id="bed-01",
                    window_start=1180, window_end_exclusive=1360, evaluated_at=1370,
                    result=dict(status="no_rollover"))) + "\n")
            result = evaluate(labels, logs)
            self.assertEqual(result["counts"]["true_positive"], 1)
            self.assertEqual(result["counts"]["true_negative"], 1)
            self.assertEqual(result["counts"]["unjudgeable"], 1)
            self.assertEqual(result["recall_conservative"], 0.5)
            self.assertEqual(result["unjudgeable_rate"], 1 / 3)
            self.assertEqual(result["mean_detection_latency_seconds"], 90)

    def test_rejects_duplicate_ground_truth(self):
        with tempfile.TemporaryDirectory() as root:
            labels = Path(root) / "labels.csv"
            logs = Path(root) / "edge.jsonl"
            labels.write_text("window_start,window_end_exclusive,label,event_at\n"
                              "0,180,no_rollover,\n0,180,no_rollover,\n", encoding="utf-8")
            logs.write_text("", encoding="utf-8")
            with self.assertRaises(ValueError):
                evaluate(labels, logs)


if __name__ == "__main__":
    unittest.main()
