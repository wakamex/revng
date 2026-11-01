#!/bin/bash
# check.sh LAUNCHER [NAME...]: build each library from src/NAME.rs, emit its C with the rev.ng launcher, compile
# it, and compare every exported function with the original on 200 random calls. With PROTOTYPES_DB set, rev.ng's
# import prototypes come from that database, linked into the launcher's resource root for the run. By default
# every library in src/ is checked.
set -u
D=$(cd "$(dirname "$0")" && pwd)
R=$1; shift; DB=${PROTOTYPES_DB:-}
RUSTC=${RUSTC:-$(command -v rustc || echo $HOME/.cargo/bin/rustc)}
names=("$@"); [ ${#names[@]} -eq 0 ] && names=($(cd $D/src && ls *.rs | sed 's/\.rs$//'))
tag=$(basename "$(dirname "$(dirname "$(dirname "$(dirname "$R")")")")")
link=$(dirname "$(dirname "$R")")/share/revng/prototypes.sqlite
if [ -n "$DB" ]; then ln -sfn "$DB" "$link"; fi
cd $D
for name in "${names[@]}"; do
  if ! $RUSTC +1.95.0 -O --edition 2024 --crate-type cdylib src/$name.rs -o $name.so; then
    echo "$name: build failed"; continue
  fi
  strip $name.so
  ./generate.sh $name "$R" > out-$name.log 2>&1
  if [ $name = corpus1 ]; then
    timeout 3000 python3 compare_corpus1.py $name.so out-$name-$tag/recovered.so --count 200 > compare-$name.json
  else
    timeout 3000 python3 compare_uniform.py $name.so out-$name-$tag/recovered.so > compare-$name.json
  fi
  python3 -c "import json; d=json.load(open('compare-$name.json'))['outcomes']; print('$name', sum(v == {'agree': 200} for v in d.values()), 'of', len(d), {k: v for k, v in d.items() if v != {'agree': 200}})" 2>/dev/null || echo "$name: no comparison, see out-$name.log"
done
if [ -n "$DB" ]; then rm -f "$link"; fi
