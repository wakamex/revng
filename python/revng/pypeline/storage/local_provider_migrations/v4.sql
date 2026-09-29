--
-- This file is distributed under the MIT License. See LICENSE.md for details.
--

-- Store object content larger than one SQLite value (about 2 GiB hard cap,
-- about 0.93 GiB by default) across several rows of `object_chunks`, keyed by
-- the owning object plus a chunk index, reassembled in order on read. Inline
-- objects keep chunk_count 0 and their content in `objects`. The caches are
-- cleared because existing rows predate the chunk_count column.

DELETE FROM objects;
DELETE FROM dependencies;
DELETE FROM custom_dependencies;

ALTER TABLE objects ADD COLUMN chunk_count INT NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS object_chunks(
    savepoint_id         INT NOT NULL,
    container_id         TEXT NOT NULL,
    configuration_hash   TEXT NOT NULL,
    object_id            BLOB NOT NULL,
    chunk_index          INT NOT NULL,
    content              BLOB NOT NULL,
    PRIMARY KEY (savepoint_id, container_id, configuration_hash, object_id,
                 chunk_index)
) STRICT;
