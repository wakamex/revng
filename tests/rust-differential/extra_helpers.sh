#!/bin/bash
# extra_helpers.sh RUNTIME_LAUNCHER OUT.o HELPER...: compile the named QEMU helpers, with what they call and the data
# they read, from the runtime's libtcg helper bitcode into one object. Everything but the helpers is internal, so it
# cannot clash with the support objects linked beside it.
set -eu
R=$1; out=$2; shift 2
rt=$(dirname "$(dirname "$R")")
B=$rt/share/revng/libtcg-helpers-full-x86_64.bc
ORCHESTRA=${ORCHESTRA:-$HOME/.local/share/revng/orchestra/root}
L=$ORCHESTRA/lib64/llvm/llvm/bin
O=$ORCHESTRA/bin
tmp=$(mktemp -d)
args=(); for h in "$@"; do args+=(-func="$h"); done
# Extract the helpers with the data they reference, adding each global the extracted code refers to that the
# bitcode defines, such as string constants and CPU state variables, until none is missing
$O/llvm-dis "$B" -o - | sed -n 's/^@"\{0,1\}\([^" ]*\)"\{0,1\} = .*/\1/p' | sort -u > $tmp/data-defined
globs=(); while :; do
  $O/llvm-extract --recursive "${args[@]}" "${globs[@]}" "$B" -o $tmp/x.bc
  new=$($O/llvm-nm --undefined-only "$tmp/x.bc" | awk '{print $NF}' | sort -u | comm -12 - $tmp/data-defined)
  [ -z "$new" ] && break
  for g in $new; do globs+=(-glob="$g"); done
done
$L/opt -passes=internalize,globaldce -internalize-public-api-list="$(IFS=,; echo "$*")" $tmp/x.bc -o $tmp/y.bc
$O/clang -c -fPIC -O2 $tmp/y.bc -o "$out"
rm -rf $tmp
