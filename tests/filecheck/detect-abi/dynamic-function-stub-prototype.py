#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Give the dynamic function `counter_next` of the model at sys.argv[1] the
# prototype `int32_t counter_next(int32_t)`, as a prototypes database would.

import sys

INT32 = "Kind: PrimitiveType\n{0}PrimitiveKind: Signed\n{0}Size: 4\n"
DEFINITION = (
    "  - ID: 9000\n"
    "    Kind: CABIFunctionDefinition\n"
    "    ABI: SystemV_x86_64\n"
    "    ReturnType:\n"
    "      " + INT32.format("      ") + "    Arguments:\n"
    "      - Index: 0\n"
    "        Type:\n"
    "          " + INT32.format("          ")
)
PROTOTYPE = (
    "    Prototype:\n"
    "      Kind: DefinedType\n"
    '      Definition: "/TypeDefinitions/9000-CABIFunctionDefinition"\n'
)

path = sys.argv[1]
with open(path) as stream:
    text = stream.read()
assert "  - Name: counter_next\n" in text
text = text.replace("  - Name: counter_next\n", "  - Name: counter_next\n" + PROTOTYPE, 1)
if "\nTypeDefinitions:\n" in text:
    text = text.replace("\nTypeDefinitions:\n", "\nTypeDefinitions:\n" + DEFINITION, 1)
else:
    text = text.rstrip("\n") + "\nTypeDefinitions:\n" + DEFINITION
with open(path, "w") as stream:
    stream.write(text)
