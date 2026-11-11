--
-- This file is distributed under the MIT License. See LICENSE.md for details.
--

-- The pipeline nodes the cached objects were computed with: the definition
-- hash of each node, which identifies it across pipelines, and the id and
-- savepoint range it had, which the cache tables refer to. When the pipeline
-- changes, the objects of the nodes whose hash is unchanged are renumbered to
-- the node's new position instead of being dropped.
CREATE TABLE IF NOT EXISTS node_definitions(
    node_id           INT PRIMARY KEY,
    definition_hash   TEXT NOT NULL,
    savepoint         INT NOT NULL,
    range_start       INT NOT NULL,
    range_end         INT NOT NULL
) STRICT;
