#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# call-site-prototype-integer-sizes.py MODEL ENTRY CALL: give the dynamic
# function `counter_next` of MODEL the prototype `int32_t (int32_t)`, and the
# call at address CALL in the function at address ENTRY the call-site
# prototype `generic64_t (generic64_t)`, as detect-abi guesses for a call whose
# callee it does not know. Addresses are hexadecimal and include the base
# address rev.ng loads the binary at.

import sys


def primitive(kind, size, indent):
    return f"Kind: PrimitiveType\n{indent}PrimitiveKind: {kind}\n{indent}Size: {size}\n"


def prototype(identifier, kind, size):
    return (
        f"  - ID: {identifier}\n"
        "    Kind: CABIFunctionDefinition\n"
        "    ABI: SystemV_x86_64\n"
        "    ReturnType:\n"
        "      " + primitive(kind, size, "      ") + "    Arguments:\n"
        "      - Index: 0\n"
        "        Type:\n"
        "          " + primitive(kind, size, "          ")
    )


def reference(identifier, indent):
    return (
        f"{indent}Prototype:\n"
        f"{indent}  Kind: DefinedType\n"
        f'{indent}  Definition: "/TypeDefinitions/{identifier}-CABIFunctionDefinition"\n'
    )


path, entry, call = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
with open(path) as stream:
    text = stream.read()

dynamic = "  - Name: counter_next\n"
assert dynamic in text
text = text.replace(dynamic, dynamic + reference(9000, "    "), 1)

function = f'  - Entry: "0x{entry:x}:Code_x86_64"\n'
assert function in text
call_site = (
    "    CallSitePrototypes:\n"
    f'      - CallerBlockAddress: "0x{call:x}:Code_x86_64"\n' + reference(9001, "        ")
)
text = text.replace(function, function + call_site, 1)

definitions = prototype(9000, "Signed", 4) + prototype(9001, "Generic", 8)
if "\nTypeDefinitions:\n" in text:
    text = text.replace("\nTypeDefinitions:\n", "\nTypeDefinitions:\n" + definitions, 1)
else:
    text = text.rstrip("\n") + "\nTypeDefinitions:\n" + definitions

with open(path, "w") as stream:
    stream.write(text)
