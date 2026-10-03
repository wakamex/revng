#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

from __future__ import annotations

import runpy
import shutil
import subprocess
from pathlib import Path

import pytest
from pipebox import MyKind, MyObjectID

import revng.pypeline
from revng.pypeline import initialize_pypeline
from revng.pypeline.storage import local_provider
from revng.pypeline.storage.local_provider import LocalStorageProvider
from revng.pypeline.storage.storage_provider import ContainerLocation, PipeDependencies
from revng.pypeline.storage.storage_provider import SavePointsRange

initialize_pypeline()

WRITE_SOURCE_ID = Path(__file__).parents[3] / "python" / "write-source-id.py"
LOCATION = ContainerLocation(0, "a", "")
ROOT = MyObjectID(MyKind.ROOT)


def write_source_id(source: Path, build: Path) -> str:
    output = build / "_source_id.py"
    subprocess.run([WRITE_SOURCE_ID, source, build, output], check=True, capture_output=True)
    return runpy.run_path(str(output))["SOURCE_ID"]


def git(directory: Path, *args: str):
    subprocess.run(["git", "-C", directory, *args], check=True, capture_output=True)


def test_source_id(tmp_path: Path):
    source = tmp_path / "source"
    build = tmp_path / "build"
    source.mkdir()
    (source / "a.c").write_text("int a;\n")
    git(source, "init", "-q")
    git(source, "add", "a.c")
    git(source, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "a")

    clean = write_source_id(source, build)
    assert clean.startswith("git-tree-") and "dirty" not in clean
    # A rebuild and a separate build of the same source give the same identity
    assert write_source_id(source, build) == clean
    assert write_source_id(source, tmp_path / "other-build") == clean
    # Files in a build directory inside the source do not count
    in_tree = source / "build"
    (in_tree / "lib.so").parent.mkdir()
    (in_tree / "lib.so").write_text("")
    assert write_source_id(source, in_tree) == clean
    shutil.rmtree(in_tree)
    # Nor does Python bytecode
    (source / "__pycache__").mkdir()
    (source / "__pycache__" / "a.cpython-314.pyc").write_bytes(b"\0")
    assert write_source_id(source, build) == clean

    # Any change to the source changes it
    (source / "a.c").write_text("int b;\n")
    edited = write_source_id(source, build)
    assert edited.startswith(clean + "-dirty-")
    (source / "a.c").write_text("int a;\n")
    assert write_source_id(source, build) == clean
    (source / "b.c").write_text("int b;\n")
    untracked = write_source_id(source, build)
    assert untracked.startswith(clean + "-dirty-") and untracked != edited

    # Committing the change gives a clean identity of the new tree
    git(source, "add", "b.c")
    git(source, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "b")
    committed = write_source_id(source, build)
    assert committed.startswith("git-tree-") and "dirty" not in committed
    assert committed != clean


def test_source_id_without_git(tmp_path: Path):
    assert write_source_id(tmp_path, tmp_path) == ""
    assert revng.pypeline.cache_version == (revng.pypeline.SOURCE_ID or revng.pypeline.__version__)


def open_with_cache_version(monkeypatch, tmp_path: Path, cache_version: str):
    monkeypatch.setattr(local_provider, "version", cache_version)
    model_path = tmp_path / "model.yml"
    model_path.touch()
    return LocalStorageProvider(tmp_path / "cache.sqlite", model_path, tmp_path, "pipeline")


def store_root(provider: LocalStorageProvider):
    provider.add_objects(
        [PipeDependencies(0, SavePointsRange(0, 0), "", [("a", ROOT, "/root")], [])],
        {LOCATION: {ROOT: b"lifted"}},
    )


@pytest.mark.parametrize(
    "second, kept",
    [
        # Two builds of the same source
        ("git-tree-1111", True),
        # A build of changed source
        ("git-tree-1111-dirty-2222", False),
        ("git-tree-3333", False),
        # A build without git metadata falls back to the package version
        ("0.1.1759500000", False),
    ],
)
def test_cache_version_prunes(monkeypatch, tmp_path: Path, second: str, kept: bool):
    store_root(open_with_cache_version(monkeypatch, tmp_path, "git-tree-1111"))
    provider = open_with_cache_version(monkeypatch, tmp_path, second)
    assert provider.has(LOCATION, [ROOT]) == ([ROOT] if kept else [])
    assert provider.metadata().version == second


def test_fallback_cache_version_prunes(monkeypatch, tmp_path: Path):
    store_root(open_with_cache_version(monkeypatch, tmp_path, "0.1.1759500000"))
    assert open_with_cache_version(monkeypatch, tmp_path, "0.1.1759500000").has(
        LOCATION, [ROOT]
    ) == [ROOT]
    assert (
        open_with_cache_version(monkeypatch, tmp_path, "0.1.1759500001").has(LOCATION, [ROOT]) == []
    )
