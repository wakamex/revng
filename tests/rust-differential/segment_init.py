"""segment_init.py MODEL BINARY OUT.c: write a constructor that copies each non-executable segment's file bytes
into the recovered C's segment_N global, so that data the C reads has the original image's initial contents, and
applies the binary's R_X86_64_RELATIVE relocations into those copies: a target in a data segment becomes the
address of that place in its segment_N, and a target at a function entry becomes the address of the recovered C
function. Targets anywhere else keep the file's bytes. Each slot an imported function's relocation fills gets the
address of that function, which the recovered library resolves from the libraries it links. The binary's thread-local
storage becomes one __thread array of the recovered library with the same initial image, and each tls_index a
R_X86_64_DTPMOD64 relocation fills names the recovered library's module and that array's place in its block."""
import re
import subprocess
import sys

import yaml

model, binary, out = sys.argv[1:]
m = yaml.safe_load(open(model))
segments = m["Segments"]
data = open(binary, "rb").read()
starts = [int(s["StartAddress"].split(":")[0], 16) for s in segments]
# rev.ng loads a position-independent image at a base; the first segment starts at it
base = starts[0] - min(int(line.split()[3], 16) for line in subprocess.run(
    ["readelf", "-lW", binary], capture_output=True, text=True, check=True).stdout.splitlines()
    if line.split()[:1] == ["LOAD"])
functions = {int(f["Entry"].split(":")[0], 16): f.get("Name") or f"function_{f['Entry'].split(':')[0]}_Code_x86_64"
             for f in m.get("Functions", [])}


def absolute64(relocation):
    """Whether a model relocation writes an object's 64-bit absolute address, in either model schema."""
    if relocation["Type"] == "WriteAbsoluteAddress64":
        return True
    return relocation["Type"] == "Absolute" and relocation.get("Size") == 8


def place(address):
    """The data segment index and offset holding an image address, or None."""
    for index, (s, start) in enumerate(zip(segments, starts)):
        if not s.get("IsExecutable") and start <= address < start + s["VirtualSize"]:
            return index, address - start
    return None


lines, externs = ["__attribute__((constructor)) static void initialize_segments(void) {"], set()
for index, s in enumerate(segments):
    size = s.get("FileSize", 0)
    payload = data[s.get("StartOffset", 0) : s.get("StartOffset", 0) + size]
    if s.get("IsExecutable") or not any(payload):
        continue
    externs.add(f"segment_{index}")
    lines += [
        f"  {{ static const unsigned char bytes[] = {{{','.join(map(str, payload))}}};",
        f"    for (unsigned long i = 0; i < {size}; ++i) segment_{index}[i] = bytes[i]; }}",
    ]
relocated = {"data": 0, "function": 0, "other": 0}
relocations = subprocess.run(["readelf", "-rW", binary], capture_output=True, text=True, check=True).stdout
for slot, addend in re.findall(r"^([0-9a-f]+)\s+[0-9a-f]+\s+R_X86_64_RELATIVE\s+([0-9a-f]+)", relocations, re.M):
    where = place(int(slot, 16) + base)
    target = int(addend, 16) + base
    if where is None:
        continue
    if (to := place(target)) is not None:
        value = f"(unsigned long)segment_{to[0]} + {to[1]}"
        externs.add(f"segment_{to[0]}")
        relocated["data"] += 1
    elif target in functions:
        value = f"(unsigned long){functions[target]}"
        externs.add(functions[target])
        relocated["function"] += 1
    else:
        relocated["other"] += 1
        continue
    externs.add(f"segment_{where[0]}")
    lines.append(f"  *(unsigned long *)(segment_{where[0]} + {where[1]}) = {value};")
for f in m.get("ImportedDynamicFunctions", []):
    for r in f.get("Relocations", []):
        where = place(int(r["Address"].split(":")[0], 16))
        if not absolute64(r) or r.get("Addend", 0) or where is None:
            relocated["other"] += 1
            continue
        externs.update({f"segment_{where[0]}", f["Name"]})
        lines.append(f"  *(unsigned long *)(segment_{where[0]} + {where[1]}) = (unsigned long){f['Name']};")
        relocated["import"] = relocated.get("import", 0) + 1
# Thread-local storage: __tls_get_addr reads a tls_index, the module and the offset in its block, which the dynamic
# loader fills in the original; in the copies the module is the file's 0
program_headers = subprocess.run(["readelf", "-lW", binary], capture_output=True, text=True, check=True).stdout
tls = [line.split() for line in program_headers.splitlines() if line.split()[:1] == ["TLS"]]
tls_lines = []
if tls:
    offset, size, memory_size, align = int(tls[0][1], 16), int(tls[0][4], 16), int(tls[0][5], 16), int(tls[0][-1], 16)
    image = data[offset : offset + size]
    tls_lines = [
        "#define _GNU_SOURCE",
        "#include <dlfcn.h>",
        f"static __thread unsigned char recovered_tls[{memory_size}] __attribute__((aligned({align}))) = "
        f"{{{','.join(map(str, image)) or '0'}}};",
    ]
    slots = re.findall(r"^([0-9a-f]+)\s+[0-9a-f]+\s+R_X86_64_DTPMOD64\s+0\s*$", relocations, re.M)
    if slots:
        lines += [
            "  { Dl_info info; size_t module = 0; void *block = 0;",
            "    unsigned char *mine = recovered_tls;",
            "    dladdr((void *)initialize_segments, &info);",
            "    void *self = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);",
            "    dlinfo(self, RTLD_DI_TLS_MODID, &module);",
            "    dlinfo(self, RTLD_DI_TLS_DATA, &block);",
            "    unsigned long start = (unsigned long)(mine - (unsigned char *)block);",
        ]
        for slot in slots:
            where = place(int(slot, 16) + base)
            if where is None:
                relocated["other"] += 1
                continue
            externs.add(f"segment_{where[0]}")
            lines += [f"    *(unsigned long *)(segment_{where[0]} + {where[1]}) = module;",
                      f"    *(unsigned long *)(segment_{where[0]} + {where[1] + 8}) += start;"]
            relocated["tls"] = relocated.get("tls", 0) + 1
        lines.append("    dlclose(self); }")
# Each symbol is declared under a neutral name bound to it, since many are C library functions the compiler knows
names = {name: f"symbol_{i}" for i, name in enumerate(sorted(externs))}
head = [f'extern unsigned char {alias}[] __asm__("{name}");' for name, alias in names.items()]
body = [re.sub(r"\b(?:segment_\d+|[A-Za-z_]\w*)\b", lambda w: names.get(w.group(0), w.group(0)), line) for line in lines]
open(out, "w").write("\n".join(tls_lines + head + body + ["}", ""]))
print("relative relocations applied:", relocated, file=sys.stderr)
