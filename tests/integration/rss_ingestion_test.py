#!/usr/bin/env python3
"""原创黄金RSS的端到端验收，独立手写正文/元数据预期，不依赖私有语料。"""

import argparse
import json
from pathlib import Path
import subprocess
import unittest

PROBE = None
RSS = None
DEFAULT = [1048576, 1048576, 1000, 8388608, 100000, 128, 1048576, 1048576, 100000, 100000, 128]


def decode(value):
    return None if value is None else bytes.fromhex(value).decode()


class RssIngestionTest(unittest.TestCase):
    def invoke(self, data, count, limits=None):
        return subprocess.run([str(PROBE), str(count), *map(str, DEFAULT if limits is None else limits)],
                              input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)

    def report(self, data, count, limits=None):
        result = self.invoke(data, count, limits)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        return json.loads(result.stdout)

    def test_original_golden_rss_exact_records_and_metadata(self):
        result = self.report(RSS.read_bytes(), 3)
        self.assertEqual(result["counts"], dict(success=1, warning=1, no_text=1, rejected=0, failed=0))
        self.assertIsNone(result["stop"])
        first, second, empty = result["items"]
        self.assertEqual([item["ordinal"] for item in result["items"]], [0, 1, 2])
        self.assertEqual(first["record"]["doc_id"], 0)
        self.assertEqual(decode(first["record"]["title_hex"]), "Copper kettle")
        self.assertEqual(decode(first["record"]["content_hex"]),
                         "A copper kettle rests on the workbench.\nThe lantern is blue.")
        self.assertEqual(decode(first["record"]["url_hex"]), "https://fixtures.example.invalid/kettle")
        self.assertEqual(decode(first["record"]["published_at_raw_hex"]), "Mon, 05 Oct 2026 09:00:00 GMT")
        self.assertIsNone(first["record"]["author_hex"])
        self.assertEqual(decode(second["record"]["content_hex"]), "纸船停在河边闸门旁。\nGreen & blue lanterns.")
        self.assertEqual(second["record"]["doc_id"], 1)
        self.assertEqual(second["issues"][0]["code"], "missing_metadata")
        self.assertEqual(empty["status"], "no_text")
        self.assertIsNone(empty["record"])
        size = sum(len(bytes.fromhex(item["record"][field])) for item in [first, second]
                   for field in ["title_hex", "content_hex"])
        self.assertEqual(result["total_text_bytes"], size)

    def test_local_content_priority_and_alias_namespace(self):
        raw = b"<rss version='2.0' xmlns:z='http://purl.org/rss/1.0/modules/content/'><channel><item><title>T</title><z:encoded>full</z:encoded><content>local</content><description>short</description></item><item><content>local</content><description>short</description></item></channel></rss>"
        result = self.report(raw, 2)
        self.assertEqual(decode(result["items"][0]["record"]["content_hex"]), "full")
        self.assertEqual(decode(result["items"][1]["record"]["content_hex"]), "local")
        self.assertEqual(decode(result["items"][1]["record"]["title_hex"]), "feed#1")

    def test_malformed_tail_and_dtd_never_publish_partial_items(self):
        raw = b"<rss version='2.0'><channel><item><description>first</description></item><item></channel></rss>"
        report = self.report(raw, 2)
        self.assertEqual(report["counts"]["failed"], 1)
        self.assertEqual(len(report["items"]), 1)
        self.assertIsNone(report["items"][0]["record"])
        dtd = b"<!DOCTYPE rss SYSTEM 'file:///not-accessed'><rss version='2.0'><channel/></rss>"
        self.assertEqual(self.report(dtd, 0)["counts"]["rejected"], 1)

    def test_batch_capacities_are_explicit_and_release_effective(self):
        limits = DEFAULT.copy()
        limits[2] = 1
        result = self.report(RSS.read_bytes(), 3, limits)
        self.assertEqual(len(result["items"]), 1)
        self.assertEqual(result["stop"], dict(reason="record_limit", ordinal=1))
        limits = DEFAULT.copy()
        limits[3] = 1
        result = self.report(RSS.read_bytes(), 3, limits)
        self.assertEqual(result["items"], [])
        self.assertEqual(result["stop"]["reason"], "total_text_limit")

    def test_preferred_limit_refusal_is_not_silently_summary(self):
        limits = DEFAULT.copy()
        limits[9] = 1
        result = self.report(RSS.read_bytes(), 3, limits)
        self.assertEqual(result["items"][0]["status"], "rejected")
        self.assertIsNone(result["items"][0]["record"])
        self.assertIn("html.nodes", decode(result["items"][0]["issues"][0]["region_hex"]))

    def test_probe_input_and_identity_configuration_are_bounded(self):
        limits = DEFAULT.copy()
        limits[0] = 1
        over = self.invoke(RSS.read_bytes(), 3, limits)
        self.assertEqual(over.returncode, 1)
        self.assertEqual(json.loads(over.stdout)["error_context"], "probe.input_bytes")
        wrong = self.invoke(RSS.read_bytes(), 2)
        self.assertEqual(wrong.returncode, 2)
        self.assertEqual(wrong.stdout, b"")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--rss", type=Path, required=True)
    args = parser.parse_args()
    PROBE = args.probe.resolve(strict=True)
    RSS = args.rss.resolve(strict=True)
    unittest.main(argv=["rss_ingestion_test"], verbosity=2)
