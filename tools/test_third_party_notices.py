"""Tests for tools/third_party_notices.py, run by tools/run_bwg_host_tests.sh."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from third_party_notices import (  # noqa: E402
    Component,
    MissingLicence,
    NpmPackage,
    collect_notices,
    licence_files,
)


class NoticesTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        self.gpl = self.write("LICENSE", "GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n")

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def write(self, relative: str, text: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def notices(self, components=(), npm_packages=(), extra_notices=()) -> str:
        return collect_notices(
            gpl_licence=self.gpl,
            commit="0123456789abcdef0123456789abcdef01234567",
            source_url="https://example.invalid/source",
            components=list(components),
            npm_packages=list(npm_packages),
            extra_notices=list(extra_notices),
        )


class HeaderTests(NoticesTestCase):
    def test_header_names_gpl_commit_and_source(self) -> None:
        # Act
        text = self.notices()

        # Assert
        header = text.split("GNU GENERAL PUBLIC LICENSE")[0]
        self.assertIn("GPL-3.0", header)
        self.assertIn("0123456789abcdef0123456789abcdef01234567", header)
        self.assertIn("https://example.invalid/source", header)
        self.assertIn("section 15", header)

    def test_includes_full_gpl_text(self) -> None:
        # Act
        text = self.notices()

        # Assert
        self.assertIn("GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n", text)


class ComponentTests(NoticesTestCase):
    def test_third_party_component_licence_appears_under_its_name(self) -> None:
        # Arrange
        self.write("lwip/lwip/COPYING", "Copyright (c) 2001 Swedish Institute\n")
        component = Component("lwip", self.root / "lwip", "third-party")

        # Act
        text = self.notices(components=[component])

        # Assert
        section = text.split("== component lwip ==")[1]
        self.assertIn("Copyright (c) 2001 Swedish Institute", section)

    def test_rejects_third_party_component_without_licence(self) -> None:
        # Arrange
        (self.root / "mystery").mkdir()
        component = Component("mystery", self.root / "mystery", "third-party")

        # Act / Assert
        with self.assertRaisesRegex(MissingLicence, "mystery"):
            self.notices(components=[component])

    def test_project_component_needs_no_licence(self) -> None:
        # Arrange
        (self.root / "bwg_worker").mkdir()
        component = Component("bwg_worker", self.root / "bwg_worker", "project")

        # Act
        text = self.notices(components=[component])

        # Assert
        self.assertNotIn("bwg_worker", text)


class NpmTests(NoticesTestCase):
    def test_npm_package_without_file_shows_declared_licence(self) -> None:
        # Arrange
        (self.root / "node_modules/zone.js").mkdir(parents=True)
        package = NpmPackage("zone.js", "0.15.0", self.root / "node_modules/zone.js", "MIT")

        # Act
        text = self.notices(npm_packages=[package])

        # Assert
        section = text.split("== npm zone.js 0.15.0 ==")[1]
        self.assertIn("declared licence: MIT; no licence file in the package", section)

    def test_rejects_npm_package_without_any_licence(self) -> None:
        # Arrange
        (self.root / "node_modules/bare").mkdir(parents=True)
        package = NpmPackage("bare", "1.0.0", self.root / "node_modules/bare", None)

        # Act / Assert
        with self.assertRaisesRegex(MissingLicence, "bare"):
            self.notices(npm_packages=[package])


class LicenceSearchTests(NoticesTestCase):
    def test_licence_search_skips_test_directories(self) -> None:
        # Arrange
        self.write("component/test/LICENSE", "test fixture licence\n")
        self.write("component/LICENSE.md", "the component's licence\n")

        # Act
        found = licence_files(self.root / "component")

        # Assert
        self.assertEqual(found, [self.root / "component/LICENSE.md"])

    def test_licence_search_reaches_two_levels(self) -> None:
        # Arrange
        self.write("component/a/b/COPYING", "two levels down\n")
        self.write("component/a/b/c/COPYING", "three levels down\n")

        # Act
        found = licence_files(self.root / "component")

        # Assert
        self.assertEqual(found, [self.root / "component/a/b/COPYING"])


if __name__ == "__main__":
    unittest.main()
