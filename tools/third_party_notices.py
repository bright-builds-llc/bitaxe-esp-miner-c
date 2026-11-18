#!/usr/bin/env python3
"""Writes the notices that go with a reference image: the GPL, and every built component's and
bundled npm package's licence, so a distributed binary carries what its licences ask for.

Usage: third_party_notices.py --build-dir DIR --web-dir DIR --commit SHA --source-url URL --output FILE
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Literal, Optional

Origin = Literal["esp-idf", "project", "third-party"]

LICENCE_PREFIXES = ("license", "licence", "copying", "notice")
SKIPPED_DIRECTORIES = {"test", "tests", "test_apps", "examples", "docs", "node_modules", ".git"}
SEARCH_DEPTH = 2
REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
NOTICES_DIR = REPOSITORY_ROOT / "reference" / "notices"


@dataclass(frozen=True)
class Component:
    name: str
    directory: Path
    origin: Origin


@dataclass(frozen=True)
class NpmPackage:
    name: str
    version: str
    directory: Path
    declared_licence: Optional[str]


class MissingLicence(Exception):
    """A third-party component or npm package that ships no licence we can pass on."""


def licence_files(directory: Path) -> list[Path]:
    """Licence-like files in a directory and up to two levels below it, skipping test and example trees."""
    found: list[Path] = []
    pending = [(directory, 0)]
    while pending:
        current, depth = pending.pop()
        for entry in current.iterdir():
            if entry.is_dir():
                if depth < SEARCH_DEPTH and entry.name.lower() not in SKIPPED_DIRECTORIES:
                    pending.append((entry, depth + 1))
            elif entry.name.lower().startswith(LICENCE_PREFIXES):
                found.append(entry)
    return sorted(found)


def _header(commit: str, source_url: str) -> str:
    return (
        "Third-party notices for the Bitaxe ESP-Miner reference image (Bright Builds fork)\n\n"
        f"This firmware is free software under the GNU General Public License, version 3 (GPL-3.0),\n"
        f"built from commit {commit}. Its Corresponding Source is at:\n{source_url}\n"
        "It comes with no warranty; see section 15 of the licence below.\n"
        "The licences of the components and packages built into it follow the GPL's text.\n\n"
    )


def _section(title: str, files: list[Path]) -> str:
    texts = [f"--- {path.name} ---\n{path.read_text(errors='replace').rstrip()}\n" for path in files]
    return f"\n== {title} ==\n" + "".join(texts)


def _component_section(component: Component) -> str:
    files = licence_files(component.directory)
    if files:
        return _section(f"component {component.name}", files)
    if component.origin == "third-party":
        raise MissingLicence(f"third-party component {component.name} has no licence file")
    return ""


def _npm_section(package: NpmPackage) -> str:
    title = f"npm {package.name} {package.version}"
    files = licence_files(package.directory)
    if files:
        return _section(title, files)
    if package.declared_licence:
        return f"\n== {title} ==\ndeclared licence: {package.declared_licence}; no licence file in the package\n"
    raise MissingLicence(f"npm package {package.name} {package.version} has no licence file or declared licence")


def collect_notices(
    *,
    gpl_licence: Path,
    commit: str,
    source_url: str,
    components: list[Component],
    npm_packages: list[NpmPackage],
    extra_notices: list[Path],
) -> str:
    """The whole notices text: header, the GPL, the extra notices, then one section per licensed item."""
    parts = [_header(commit, source_url), gpl_licence.read_text()]
    parts += [_section(f"notice {path.name}", [path]) for path in extra_notices]
    parts += [_component_section(component) for component in sorted(components, key=lambda c: c.name)]
    parts += [_npm_section(package) for package in sorted(npm_packages, key=lambda p: (p.name, p.version))]
    return "".join(parts)


def _origin(directory: Path, project: Path, idf: Path) -> Origin:
    if directory.is_relative_to(idf):
        return "esp-idf"
    if directory.is_relative_to(project / "managed_components") or not directory.is_relative_to(project):
        return "third-party"
    return "project"


def components_from_build(build_dir: Path) -> tuple[list[Component], Path, Path]:
    """The built components named by ESP-IDF's project description, with the project and IDF roots."""
    description = json.loads((build_dir / "project_description.json").read_text())
    project = Path(description["project_path"]).resolve()
    idf = Path(description["idf_path"]).resolve()
    components = []
    for name, info in description["build_component_info"].items():
        directory = Path(info["dir"]).resolve()
        components.append(Component(name, directory, _origin(directory, project, idf)))
    return components, project, idf


def _npm_package(directory: Path) -> NpmPackage:
    manifest = json.loads((directory / "package.json").read_text())
    declared = manifest.get("license")
    if isinstance(declared, dict):
        declared = declared.get("type")
    return NpmPackage(manifest["name"], manifest["version"], directory, declared)


def npm_packages_from_web(web_dir: Path, bundled_dev_packages: list[str]) -> list[NpmPackage]:
    """Production npm packages per `npm ls`, plus dev packages whose code still ships in the bundle."""
    listing = subprocess.run(
        ["npm", "ls", "--omit=dev", "--all", "--parseable"],
        cwd=web_dir, check=True, capture_output=True, text=True,
    ).stdout.split()
    directories = {Path(line).resolve() for line in listing} - {web_dir.resolve()}
    directories |= {(web_dir / "node_modules" / name).resolve() for name in bundled_dev_packages}
    return [_npm_package(directory) for directory in sorted(directories)]


def _bundled_dev_packages() -> list[str]:
    lines = (NOTICES_DIR / "bundled-dev-packages.txt").read_text().splitlines()
    return [line.strip() for line in lines if line.strip() and not line.startswith("#")]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for option in ("--build-dir", "--web-dir", "--output"):
        parser.add_argument(option, type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--source-url", required=True)
    arguments = parser.parse_args(argv)

    components, _, idf = components_from_build(arguments.build_dir)
    text = collect_notices(
        gpl_licence=REPOSITORY_ROOT / "LICENSE",
        commit=arguments.commit,
        source_url=arguments.source_url,
        components=components,
        npm_packages=npm_packages_from_web(arguments.web_dir, _bundled_dev_packages()),
        extra_notices=[idf / "LICENSE", idf / "docs" / "en" / "COPYRIGHT.rst", NOTICES_DIR / "bitaxe-esp-miner.txt"],
    )
    arguments.output.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
