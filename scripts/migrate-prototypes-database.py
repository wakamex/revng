#!/usr/bin/env python3
#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

"""Migrate a prototypes database (share/revng/prototypes.sqlite) to the model schema of this rev.ng.

ImportPrototypesFromDatabase composes each library's header, type definitions and imported functions into one model
and parses it, so a database written for an older schema imports nothing. This rebuilds each library's model the same
way, runs rev.ng's model migrations on it, and writes the migrated header, type definitions and symbol bodies to a
copy of the database.

Usage: migrate-prototypes-database.py INPUT OUTPUT [--jobs N]
"""

import argparse
import itertools
import shutil
import sqlite3
from multiprocessing import Pool

import yaml

from revng.model import Binary  # type: ignore
from revng.model.migrations import migrate

Loader = getattr(yaml, "CSafeLoader", yaml.SafeLoader)
Dumper = getattr(yaml, "CSafeDumper", yaml.SafeDumper)


def dump(value):
    return yaml.dump(value, Dumper=Dumper, default_flow_style=False, sort_keys=False)


def migrate_library(row):
    """Migrate one library: (library ID, header, [(type ID, body)], [(symbol ID, name, body)])."""
    library_id, header, types, symbols = row
    model = yaml.load(header, Loader=Loader) or {}
    model["TypeDefinitions"] = [yaml.load(body, Loader=Loader) for _, body in types]
    model["ImportedDynamicFunctions"] = [
        {"Name": name, **(yaml.load(body, Loader=Loader) or {})} for _, name, body in symbols
    ]
    migrate(model)

    migrated_types = model.pop("TypeDefinitions")
    migrated_symbols = model.pop("ImportedDynamicFunctions")
    assert len(migrated_types) == len(types) and len(migrated_symbols) == len(symbols), library_id
    type_rows = [(dump(body), type_id) for (type_id, _), body in zip(types, migrated_types)]
    symbol_rows = []
    for (symbol_id, name, body), function in zip(symbols, migrated_symbols):
        assert function.pop("Name") == name, (library_id, name)
        symbol_rows.append((dump(function) if function else "", symbol_id))
    return library_id, dump(model), type_rows, symbol_rows


def grouped(rows):
    """Group (library ID, ...) rows, which a database stores library by library, by library ID."""
    seen = set()
    for library_id, group in itertools.groupby(rows, key=lambda row: row[0]):
        assert library_id not in seen, f"rows of library {library_id} are not contiguous"
        seen.add(library_id)
        yield library_id, [row[1:] for row in group]


def libraries(connection):
    """Each library with its type definitions and symbols. TypeDefinition has no index on LibraryID, so both tables
    are read in one pass each, in ID order."""
    types = grouped(
        connection.execute("SELECT LibraryID, TypeDefinitionID, Body FROM TypeDefinition ORDER BY TypeDefinitionID")
    )
    symbols = grouped(connection.execute("SELECT LibraryID, SymbolID, Name, Body FROM Symbol ORDER BY SymbolID"))
    next_types, next_symbols = next(types, None), next(symbols, None)
    for (library_id, header) in connection.execute("SELECT LibraryID, Header FROM Library ORDER BY LibraryID"):
        library_types = library_symbols = []
        if next_types and next_types[0] == library_id:
            library_types, next_types = next_types[1], next(types, None)
        if next_symbols and next_symbols[0] == library_id:
            library_symbols, next_symbols = next_symbols[1], next(symbols, None)
        yield library_id, header, library_types, library_symbols
    assert next_types is None and next_symbols is None, "rows of a library not in the Library table"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()

    shutil.copyfile(args.input, args.output)
    # The pool reads the libraries from its task feeder thread
    source = sqlite3.connect(f"file:{args.input}?mode=ro", uri=True, check_same_thread=False)
    destination = sqlite3.connect(args.output)
    count = source.execute("SELECT COUNT(*) FROM Library").fetchone()[0]
    with Pool(args.jobs) as pool, destination:
        for done, (library_id, header, type_rows, symbol_rows) in enumerate(
            pool.imap(migrate_library, libraries(source), chunksize=8), 1
        ):
            destination.execute("UPDATE Library SET Header = ? WHERE LibraryID = ?", (header, library_id))
            destination.executemany("UPDATE TypeDefinition SET Body = ? WHERE TypeDefinitionID = ?", type_rows)
            destination.executemany("UPDATE Symbol SET Body = ? WHERE SymbolID = ?", symbol_rows)
            if done % 500 == 0:
                print(f"{done} of {count} libraries", flush=True)
    destination.execute("VACUUM")
    print(f"migrated {count} libraries to model version {Binary.SchemaVersion}")


if __name__ == "__main__":
    main()
