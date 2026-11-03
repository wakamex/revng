"""Generate connected C and headers with rev.ng, then compile the replacement.

No recovered body or type is edited. The upstream PTML parser removes markup;
the distribution's own support headers define its C annotations and types.
Prepared models use the engine's schema migration when their version differs.
With --intrinsic-wrappers, the LLVM intrinsics that the C calls as helpers
link against generated IR wrappers.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

import revng_intrinsic_wrappers


def initialize_data(model_path, binary, output):
    """Initialize declared data and imported pointers from the prepared model."""
    import yaml

    model = yaml.safe_load(model_path.read_text())
    data = binary.read_bytes()
    lines = [
        '#include "types-and-globals.h"',
        "__attribute__((constructor)) static void initialize_slice(void) {",
    ]
    segments = model.get("Segments", [])
    for s in segments:
        # An image-resident segment names the loaded original image's bytes;
        # writing them here would overwrite the image's state.
        if s.get("IsExecutable") or s.get("IsImageResident"):
            continue
        if s.get("Binary") != "/Binaries/0" or not s.get("Name"):
            raise ValueError(
                "prepared data segment lacks binary identity or stable name"
            )
        start, size = s.get("StartOffset", 0), s.get("FileSize", 0)
        payload = data[start : start + size]
        if len(payload) != size:
            raise ValueError("prepared segment exceeds the analysis image")
        if any(payload):
            literal = ",".join(str(b) for b in payload)
            lines += [
                f"  {{ const unsigned char bytes[] = {{{literal}}};",
                f"    unsigned char *dest = (unsigned char *)&{s['Name']};",
                f"    for (unsigned long i=0; i<{size}; ++i) dest[i]=bytes[i]; }}",
            ]
        if s.get("Relocations"):
            raise ValueError("segment relocations need explicit producer support")
    for f in model.get("ImportedDynamicFunctions", []):
        for r in f.get("Relocations", []):
            # The model schema before rev.ng's 2026-09 relocation change, and after it
            absolute64 = r["Type"] == "WriteAbsoluteAddress64" or (
                r["Type"] == "Absolute" and r.get("Size") == 8
            )
            if not absolute64 or r.get("Addend", 0):
                raise ValueError("unsupported imported relocation")
            address = int(r["Address"].split(":")[0], 16)
            s = next(
                s
                for s in segments
                if int(s["StartAddress"].split(":")[0], 16) <= address
                and address + 8
                <= int(s["StartAddress"].split(":")[0], 16) + s["VirtualSize"]
            )
            if s.get("IsExecutable") or s.get("IsImageResident"):
                raise ValueError("import relocation overlaps code or image data")
            delta = address - int(s["StartAddress"].split(":")[0], 16)
            lines += [
                f"  {{ uintptr_t pointer = (uintptr_t)&{f['Name']};",
                f"    unsigned char *dest = (unsigned char *)&{s['Name']} + {delta};",
                "    for (unsigned i=0; i<8; ++i) dest[i]=(unsigned char)(pointer >> (8*i)); }",
            ]
    lines += ["}", ""]
    destination = output / "initialize-data.c"
    destination.write_text("\n".join(lines))
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--revng", type=Path, required=True)
    parser.add_argument(
        "--cc",
        type=Path,
        help="C compiler that handles the generated C (Fedora clang 22.1.8); "
        "compiles natively against the host libc",
    )
    parser.add_argument(
        "--zig",
        type=Path,
        help="Fallback: zig, invoked as 'zig cc' cross-targeting a pinned glibc. "
        "Its bundled clang 20.1.2 crashes on some generated C, so prefer --cc",
    )
    parser.add_argument("--model", type=Path, help="Prepared, hash-pinned rev.ng model")
    parser.add_argument(
        "--link",
        action="append",
        type=Path,
        default=[],
        help="Explicit implementation object or C source for declared imports",
    )
    parser.add_argument(
        "--link-flag",
        action="append",
        default=[],
        help="Linker argument for a support library, such as -lm; recorded in the report",
    )
    parser.add_argument(
        "--intrinsic-wrappers",
        action="store_true",
        help="Link generated wrappers for the LLVM intrinsics helpers.h declares",
    )
    args = parser.parse_args()
    if bool(args.cc) == bool(args.zig):
        parser.error("pass exactly one of --cc or --zig")
    args.output.mkdir(exist_ok=False)
    shutil.copyfile(__file__, args.output / "run_revng_control.py")
    project = args.output / "project"
    project.mkdir()
    environment = dict(os.environ, TMPDIR=str(args.output / "tmp"))
    Path(environment["TMPDIR"]).mkdir()
    report = {
        "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "manual_semantic_edits": 0,
        "complete": False,
        "commands": [],
    }

    def run(command):
        started = time.monotonic()
        result = subprocess.run(
            [str(x) for x in command],
            cwd=project,
            env=environment,
            capture_output=True,
            text=True,
            timeout=14400,
            check=False,
        )
        report["commands"].append(
            {
                "argv": [str(x) for x in command],
                "returncode": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr,
                "seconds": time.monotonic() - started,
            }
        )
        result.check_returncode()

    try:
        prefix = [args.revng, "project", "--storage-provider", "local://?inline"]
        if args.model:
            import yaml

            run([*prefix, "init", "--no-initial-auto-analysis", args.binary])
            supplied_version = yaml.safe_load(args.model.read_text())["Version"]
            engine_version = yaml.safe_load((project / "revng.yml").read_text())[
                "Version"
            ]
            if supplied_version == engine_version:
                shutil.copyfile(args.model, project / "revng.yml")
            else:
                run(
                    [
                        args.revng,
                        "model",
                        "migrate",
                        args.model,
                        "-o",
                        project / "revng.yml",
                    ]
                )
            report["model_sha256"] = hashlib.sha256(args.model.read_bytes()).hexdigest()
            report["migrated_model_sha256"] = hashlib.sha256(
                (project / "revng.yml").read_bytes()
            ).hexdigest()
            for analysis in (
                "verify-model",
                "detect-abi",
                "detect-stack-size",
                "analyze-data-layout",
                "convert-functions-to-cabi",
            ):
                run(
                    [
                        *prefix,
                        "analyze",
                        analysis,
                        "-o",
                        args.output / (analysis + ".yml"),
                    ]
                )
        else:
            run([*prefix, "init", args.binary])
        for artifact, name in [
            ("emit-c-as-single-file", "recovered.c"),
            ("emit-type-and-global-header", "types-and-globals.h"),
            ("emit-helper-header", "helpers.h"),
        ]:
            raw = args.output / (name + ".ptml")
            run([*prefix, "artifact", artifact, "-o", raw])
            run([args.revng, "ptml", "--plain", raw, "-o", args.output / name])
        # Supported engines scalarize these CPU-state accesses before emitting C.
        # Linking helpers cannot restore register values lost during analysis.
        residual = sorted(
            set(
                re.findall(
                    r"\b(helper_gvec_\w+)\s*\(",
                    (args.output / "recovered.c").read_text(),
                )
            )
        )
        if residual:
            report["unsupported_helpers"] = residual
            raise ValueError(
                "unreconstructed SIMD CPU-state accesses: " + ", ".join(residual)
            )
        initialization = []
        if args.model:
            initialization = [
                initialize_data(project / "revng.yml", args.binary, args.output)
            ]
        wrappers = []
        if args.intrinsic_wrappers:
            shutil.copyfile(
                revng_intrinsic_wrappers.__file__,
                args.output / "revng_intrinsic_wrappers.py",
            )
            text = revng_intrinsic_wrappers.module(
                [(args.output / "helpers.h.ptml").read_text()]
            )
            if text:
                (args.output / "intrinsic-wrappers.ll").write_text(text)
                wrappers = [
                    "-Wno-override-module",
                    args.output / "intrinsic-wrappers.ll",
                ]
        # zig cross-targets a pinned glibc; a native clang builds for the host libc
        # it runs on. clang 22.1.8 handles generated C that crashes zig's clang 20.
        compiler = (
            [args.cc]
            if args.cc
            else [args.zig, "cc", "-target", "x86_64-linux-gnu.2.39"]
        )
        run(
            [
                *compiler,
                "-shared",
                "-fPIC",
                "-O2",
                "-fno-strict-aliasing",
                "-fwrapv",
                "-fcommon",
                "-Wl,--no-undefined",
                "-I",
                args.revng.parent / "root/share/revng/include",
                args.output / "recovered.c",
                *initialization,
                *wrappers,
                *args.link,
                *args.link_flag,
                "-o",
                args.output / "recovered.so",
            ]
        )
        report["complete"] = True
    except Exception as error:
        report["failure"] = {"type": type(error).__name__, "message": str(error)}
        raise
    finally:
        report["linked_implementations"] = {
            str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in args.link
        }
        report["link_flags"] = args.link_flag
        report["artifacts"] = {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in args.output.iterdir()
            if p.is_file()
        }
        report["script_sha256"] = hashlib.sha256(
            Path(__file__).read_bytes()
        ).hexdigest()
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
