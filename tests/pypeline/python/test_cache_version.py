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
from revng.pypeline.utils.pipeline import NodeDefinition

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
    assert revng.pypeline.cache_version == revng.pypeline.combine_cache_version(
        revng.pypeline.SOURCE_ID, revng.pypeline.RUNTIME_ID, revng.pypeline.__version__
    )


def test_combine_cache_version():
    combine = revng.pypeline.combine_cache_version
    assert combine("git-tree-1", "", "0.1.5") == "git-tree-1"
    assert combine("", "", "0.1.5") == "0.1.5"
    assert combine("git-tree-1", "runtime-2", "0.1.5") == "git-tree-1+runtime-2"
    assert combine("", "runtime-2", "0.1.5") == "0.1.5+runtime-2"
    # A runtime identity separates runtimes built from the same source
    assert combine("git-tree-1", "runtime-2", "0.1.5") != combine(
        "git-tree-1", "runtime-3", "0.1.5"
    )


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
        # The same source with other dependencies
        ("git-tree-1111+runtime-4444", False),
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


def open_with_nodes(tmp_path: Path, pipeline_hash: str, nodes: list[NodeDefinition] | None):
    model_path = tmp_path / "model.yml"
    model_path.touch()
    return LocalStorageProvider(
        tmp_path / "cache.sqlite", model_path, tmp_path, pipeline_hash, nodes
    )


def pipeline_nodes(first: str = "a", second: str = "b", swapped: bool = False, pipe: str = "p"):
    """A pipe (node 0) feeding two savepoints, which a change elsewhere in the pipeline can swap"""
    a, b = (2, 1) if swapped else (1, 2)
    return [
        NodeDefinition(pipe, 0, False, 0, 2),
        NodeDefinition(first, a, True, a, a),
        NodeDefinition(second, b, True, b, b),
    ]


def store_both(provider: LocalStorageProvider, custom: bool = False):
    provider.add_objects(
        [
            PipeDependencies(
                0,
                SavePointsRange(0, 2),
                "",
                [("a", ROOT, "/root"), ("b", ROOT, "/root")],
                [[(ROOT, b"data")]] if custom else [],
            )
        ],
        {
            ContainerLocation(1, "a", ""): {ROOT: b"first"},
            ContainerLocation(2, "b", ""): {ROOT: b"second"},
        },
    )


def stored(provider: LocalStorageProvider, savepoint: int, container: str) -> bytes | None:
    location = ContainerLocation(savepoint, container, "")
    if provider.has(location, [ROOT]) != [ROOT]:
        return None
    return bytes(provider.get(location, [ROOT])[ROOT])


def test_unchanged_nodes_are_renumbered(tmp_path: Path):
    store_both(open_with_nodes(tmp_path, "p1", pipeline_nodes()))
    provider = open_with_nodes(tmp_path, "p2", pipeline_nodes(swapped=True))
    assert stored(provider, 2, "a") == b"first"
    assert stored(provider, 1, "b") == b"second"
    assert stored(provider, 1, "a") is None


def test_changed_savepoint_is_dropped(tmp_path: Path):
    store_both(open_with_nodes(tmp_path, "p1", pipeline_nodes()))
    provider = open_with_nodes(tmp_path, "p2", pipeline_nodes(second="c", swapped=True))
    assert stored(provider, 2, "a") == b"first"
    assert stored(provider, 1, "b") is None


def test_objects_guarded_by_a_changed_pipe_are_dropped(tmp_path: Path):
    # The savepoints keep their hashes here only to show that the pipe's dependencies alone decide
    store_both(open_with_nodes(tmp_path, "p1", pipeline_nodes()), custom=True)
    provider = open_with_nodes(tmp_path, "p2", pipeline_nodes(pipe="q"))
    assert stored(provider, 1, "a") is None
    assert stored(provider, 2, "b") is None
    with provider._cursor() as cursor:
        cursor.execute("SELECT count(*) FROM dependency_keys")
        assert cursor.fetchone() == (0,)
        cursor.execute("SELECT count(*) FROM custom_dependencies")
        assert cursor.fetchone() == (0,)


def test_inserted_savepoint_renumbers_what_follows(tmp_path: Path):
    store_both(open_with_nodes(tmp_path, "p1", pipeline_nodes()), custom=True)
    provider = open_with_nodes(
        tmp_path,
        "p2",
        [
            NodeDefinition("x", 0, True, 1, 1),
            NodeDefinition("p", 1, False, 1, 3),
            NodeDefinition("a", 2, True, 2, 2),
            NodeDefinition("b", 3, True, 3, 3),
        ],
    )
    assert stored(provider, 2, "a") == b"first"
    assert stored(provider, 3, "b") == b"second"
    assert provider.get_custom_invalidation_data(1, "") == [[(ROOT, b"data")]]
    with provider._cursor() as cursor:
        cursor.execute("SELECT savepoint_id_start, savepoint_id_end FROM dependency_keys")
        assert cursor.fetchall() == [(1, 3), (1, 3)]


def test_pipeline_change_without_stored_nodes_prunes_all(tmp_path: Path):
    store_both(open_with_nodes(tmp_path, "p1", None))
    provider = open_with_nodes(tmp_path, "p2", pipeline_nodes())
    assert stored(provider, 1, "a") is None
    assert stored(provider, 2, "b") is None
