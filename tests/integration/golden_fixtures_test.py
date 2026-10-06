"""Verify original fixture bytes and independent relationships, without importing business code."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import unittest
import xml.etree.ElementTree as ET


def validate_fixture(root, project_license):
    if root.is_symlink() or not root.is_dir():
        raise ValueError("fixture root must be a real directory")
    actual = set()

    def scan_error(error):
        raise error

    for here, directories, filenames in os.walk(root, onerror=scan_error, followlinks=False):
        for name in directories + filenames:
            path = Path(here) / name
            if path.is_symlink() or not (path.is_file() or path.is_dir()):
                raise ValueError("fixture contains a linked or special object")
            if path.is_file():
                actual.add(path.relative_to(root).as_posix())
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if type(manifest["schema_version"]) is not int or manifest["schema_version"] != 1:
        raise ValueError("unknown fixture schema")
    if manifest["origin"] != "original-siftwing-fixture" or manifest["license"] != "MIT":
        raise ValueError("unexpected declared provenance or license")
    if (root / "LICENSE").read_bytes() != project_license.read_bytes():
        raise ValueError("fixture license differs from project license")
    names = []
    data = {}
    for entry in manifest["files"]:
        name = entry["path"]
        path = PurePosixPath(name)
        if path.is_absolute() or any(p in ("", ".", "..") for p in name.split("/")):
            raise ValueError("unsafe fixture path")
        if path.parts[0] not in ("txt", "rss") or path.as_posix() != name:
            raise ValueError("unexpected fixture path")
        names.append(name)
        content = (root / name).read_bytes()
        if type(entry["size_bytes"]) is not int or len(content) != entry["size_bytes"]:
            raise ValueError("fixture size differs: " + name)
        if hashlib.sha256(content).hexdigest() != entry["sha256"]:
            raise ValueError("fixture hash differs: " + name)
        content.decode("utf-8", errors="strict")
        if content.startswith(b"\xef\xbb\xbf") or b"\r" in content or (content and not content.endswith(b"\n")):
            raise ValueError("fixture UTF-8/LF convention differs: " + name)
        data[name] = content
    if names != sorted(set(names)):
        raise ValueError("fixture list must be sorted and unique")
    if actual != set(names) | {"README.md", "LICENSE", "manifest.json"}:
        raise ValueError("fixture file set differs")
    for first, second in manifest["exact_duplicates"]:
        if first == second or not data[first] or data[first] != data[second]:
            raise ValueError("declared duplicate relationship differs")
    if {name for name, content in data.items() if not content} != set(manifest["empty_files"]):
        raise ValueError("declared empty files differ")
    rss = manifest["rss"]
    xml = data[rss["path"]]
    if b"<!DOCTYPE" in xml or b"<!ENTITY" in xml:
        raise ValueError("fixture must not contain DTD or entity declarations")
    feed = ET.fromstring(xml)
    if feed.tag != "rss" or feed.get("version") != "2.0":
        raise ValueError("fixture is not RSS 2.0")
    items = feed.findall("./channel/item")
    expected = rss["items"]
    if len(items) != len(expected):
        raise ValueError("RSS item count differs")
    encoded = "{http://purl.org/rss/1.0/modules/content/}encoded"
    for item, fields in zip(items, expected):
        for field, value in fields.items():
            tag = encoded if field == "content:encoded" else field
            if item.findtext(tag) != value:
                raise ValueError("RSS raw field differs: " + field)
    return len(names), len(items)


class GoldenFixtureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="siftwing-golden-test-")
        self.addCleanup(self.temp.cleanup)
        self.copy = Path(self.temp.name) / "golden"
        shutil.copytree(FIXTURES, self.copy)

    def validate_copy(self):
        return validate_fixture(self.copy, PROJECT_LICENSE)

    def rehash(self, name):
        path = self.copy / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        content = (self.copy / name).read_bytes()
        for entry in manifest["files"]:
            if entry["path"] == name:
                entry.update(size_bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
        path.write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

    def test_original_fixture_contract(self):
        self.assertEqual(validate_fixture(FIXTURES, PROJECT_LICENSE), (7, 3))

    def test_same_size_changed_bytes_are_detected(self):
        path = self.copy / "txt/en-river.txt"
        path.write_bytes(path.read_bytes().replace(b"green", b"amber"))
        with self.assertRaisesRegex(ValueError, "hash differs"):
            self.validate_copy()

    def test_missing_input_is_detected(self):
        (self.copy / "txt/empty.txt").unlink()
        with self.assertRaises(FileNotFoundError):
            self.validate_copy()

    def test_extra_input_is_detected(self):
        (self.copy / "txt/extra.txt").write_text("extra\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "file set differs"):
            self.validate_copy()

    def test_unsafe_manifest_path_is_detected(self):
        path = self.copy / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["files"][0]["path"] = "../outside"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "unsafe fixture path"):
            self.validate_copy()

    def test_linked_input_is_detected(self):
        path = self.copy / "txt/en-river.txt"
        path.unlink()
        path.symlink_to(FIXTURES / "txt/en-river.txt")
        with self.assertRaisesRegex(ValueError, "linked or special"):
            self.validate_copy()

    def test_duplicate_expectation_is_independent_of_hash(self):
        name = "txt/en-workbench-copy.txt"
        path = self.copy / name
        path.write_bytes(path.read_bytes().replace(b"blue", b"pink"))
        self.rehash(name)
        with self.assertRaisesRegex(ValueError, "duplicate relationship differs"):
            self.validate_copy()

    def test_rss_expectation_is_independent_of_hash(self):
        name = "rss/original.xml"
        path = self.copy / name
        path.write_bytes(path.read_bytes().replace(b"Copper kettle", b"Silver kettle"))
        self.rehash(name)
        with self.assertRaisesRegex(ValueError, "RSS raw field differs"):
            self.validate_copy()

    def test_license_copy_must_match_project(self):
        (self.copy / "LICENSE").write_text("different license\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "license differs"):
            self.validate_copy()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--project-license", required=True, type=Path)
    args = parser.parse_args()
    FIXTURES = args.fixtures
    PROJECT_LICENSE = args.project_license
    unittest.main(argv=["golden_fixtures_test"], verbosity=2)
