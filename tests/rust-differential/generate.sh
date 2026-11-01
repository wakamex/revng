#!/bin/bash
# generate.sh NAME RUNTIME_LAUNCHER: emit C for NAME.so with rev.ng and compile it into out-NAME-RUNTIME/recovered.so,
# with the hl-node project's runtime library (rr_* operations), generated LLVM intrinsic wrappers, the QEMU helpers,
# the import shims in imports.c, the segments' initial bytes from the binary (segment_init.py), and aborting stubs for
# the wrappers of helpers rev.ng does not inline, whose bodies the C does not contain.
set -u
D=$(cd "$(dirname "$0")" && pwd)
S=$D/support
ORCHESTRA=${ORCHESTRA:-$HOME/.local/share/revng/orchestra/root}
export ORCHESTRA
name=$1; R=$2; tag=$(basename "$(dirname "$(dirname "$(dirname "$(dirname "$R")")")")")
out=$D/out-$name-$tag; rm -rf "$out"
# QEMU helpers the recovered C calls that the support objects do not define
$D/extra_helpers.sh "$R" $D/extra-helpers-$tag.o helper_cc_compute_c helper_cc_compute_all helper_packsswb_xmm
PYTHONPATH=$S $ORCHESTRA/bin/python3 $D/run_revng_control.py \
  $D/$name.so "$out" --revng "$R" --cc "$(which clang)" --intrinsic-wrappers \
  --link $S/revng_runtime_library.c --link $S/tcg-with-data.o --link $S/revng_runtime_traps.c \
  --link $D/imports.c --link $D/extra-helpers-$tag.o
# The harness initializes segments only from a prepared model and does not define helper wrappers: relink with the
# segments' bytes and the stubs, whether or not the harness's own link succeeded
python3 $D/segment_init.py "$out/project/revng.yml" $D/$name.so "$out/segment-init.c" && python3 - "$out" "$R" "$D" "$tag" <<'PY'
import json, re, subprocess, sys
out, launcher, here, tag = sys.argv[1:5]
names = sorted(set(re.findall(r"\b(helper_\w+_wrapper)\(", open(f"{out}/helpers.h").read())))
stubs = ["#include <math.h>", "#include <stdio.h>", "#include <stdlib.h>"] + [
    f'void {n}(void) {{ fputs("unsupported helper wrapper {n}\\n", stderr); abort(); }}' for n in names]
# A stack pointer placeholder rev.ng leaves where it could not segregate a stack access
stubs.append("unsigned long revng_undefined_local_sp(void) { return 0; }")
# QEMU's CPUID emulation, which helper_cpuid calls: the host's answers, which the original code sees too
stubs.append("void cpu_x86_cpuid(void *env, unsigned index, unsigned count, unsigned *a, unsigned *b, unsigned *c, "
             "unsigned *d) { (void)env; __asm__ volatile(\"cpuid\" : \"=a\"(*a), \"=b\"(*b), \"=c\"(*c), \"=d\"(*d) "
             ": \"a\"(index), \"c\"(count)); }")
# glibc's old classification entry points, which QEMU's softfloat calls and current glibc keeps for existing
# binaries only
stubs.append("int __fpclassify(double x) { return fpclassify(x); }")
stubs.append("int __fpclassifyf(float x) { return fpclassify(x); }")
open(f"{out}/wrapper-stubs.c", "w").write("\n".join(stubs) + "\n")
argv = json.load(open(f"{out}/report.json"))["commands"][-1]["argv"]
argv[argv.index("-o"):argv.index("-o")] = [f"{out}/segment-init.c", f"{out}/wrapper-stubs.c"]
# libm, for the softfloat routines extracted below
argv.append("-lm")
result = subprocess.run(argv, capture_output=True, text=True)
# Functions the C calls that the runtime's helper bitcode defines, such as QEMU's softfloat routines that the
# floating point helpers call: compile them from the bitcode and link again
missing = sorted(set(re.findall(r"undefined reference to `([^']+)'", result.stderr)))
if result.returncode and missing:
    rt = __import__("os").path.dirname(__import__("os").path.dirname(launcher))
    defined = set(subprocess.run([__import__("os").environ["ORCHESTRA"] + "/bin/llvm-nm", "--defined-only",
                                  f"{rt}/share/revng/libtcg-helpers-full-x86_64.bc"], capture_output=True,
                                 text=True).stdout.split())
    extract = [m for m in missing if m in defined]
    if extract:
        extra = f"{out}/extra-from-bitcode.o"
        subprocess.run([f"{here}/extra_helpers.sh", launcher, extra, *extract], check=True)
        argv[argv.index("-o"):argv.index("-o")] = [extra]
        result = subprocess.run(argv, capture_output=True, text=True)
        print("compiled from the helper bitcode:", " ".join(extract))
print("stubbed wrappers:", " ".join(names) or "none")
if result.returncode:
    print("\n".join(l for l in result.stderr.splitlines() if "undefined reference" in l or "error" in l)[:3000])
sys.exit(result.returncode)
PY
rc=$?
echo "rc=$rc out=$out"
