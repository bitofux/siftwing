#!/usr/bin/env python3
"""原创RSS字段→HTML探针的独立验收；不实现生产RSS策略，不依赖私有语料。"""

import argparse
import json
from pathlib import Path
import subprocess
import unittest
import xml.etree.ElementTree as ET

PROBE = None
RSS = None


class HtmlFragmentIntegrationTest(unittest.TestCase):
    def invoke(self, fragment, limits=(1048576, 1048576, 100000, 100000, 256)):
        return subprocess.run([str(PROBE), *map(str, limits)], input=fragment.encode(),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)

    def extract(self, fragment):
        result = self.invoke(fragment)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        report = json.loads(result.stdout)
        return bytes.fromhex(report["text_hex"]).decode(), report["recovered"]

    def test_original_rss_fields_have_independent_text_expectations(self):
        # 只读既有原创固定XML，Python取字段用于本模块集成；优先/回退策略留给M1.09。
        raw = RSS.read_bytes()
        self.assertNotIn(b"<!DOCTYPE", raw.upper())
        items = ET.fromstring(raw).findall("./channel/item")
        content = items[0].find("{http://purl.org/rss/1.0/modules/content/}encoded").text
        self.assertEqual(self.extract(content),
                         ("A copper kettle rests on the workbench.\nThe lantern is blue.", False))
        self.assertEqual(self.extract(items[0].find("description").text),
                         ("A fallback description for the kettle.", False))
        self.assertEqual(self.extract(items[1].find("description").text),
                         ("纸船停在河边闸门旁。\nGreen & blue lanterns.", False))
        self.assertEqual(self.extract(items[2].find("description").text), ("", False))

    def test_repeat_ownership_recovery_and_exact_output_bound(self):
        fragment = "<p>A</bogus>B<script>not text</script>"
        self.assertEqual(self.extract(fragment), ("AB", True))
        self.assertEqual(self.extract(fragment), self.extract(fragment))
        exact = self.invoke("<p>甲</p><p>乙</p>", limits=(64, 7, 64, 64, 8))
        self.assertEqual(exact.returncode, 0)
        self.assertEqual(bytes.fromhex(json.loads(exact.stdout)["text_hex"]).decode(), "甲\n乙")
        too_small = self.invoke("<p>甲</p><p>乙</p>", limits=(64, 6, 64, 64, 8))
        self.assertEqual(too_small.returncode, 1)
        self.assertEqual(bytes.fromhex(json.loads(too_small.stdout)["error_context_hex"]), b"html.output_bytes")

    def test_probe_input_budget_and_configuration(self):
        over = self.invoke("long", limits=(3, 10, 10, 10, 10))
        self.assertEqual(over.returncode, 1)
        self.assertEqual(json.loads(over.stdout)["error_context"], "html.input_bytes")
        invalid = self.invoke("x", limits=(0, 10, 10, 10, 10))
        self.assertEqual(invalid.returncode, 2)
        self.assertEqual(invalid.stdout, b"")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--rss", type=Path, required=True)
    args = parser.parse_args()
    PROBE = args.probe.resolve(strict=True)
    RSS = args.rss.resolve(strict=True)
    unittest.main(argv=["html_fragment_integration_test"], verbosity=2)
