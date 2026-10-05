--
-- This file is distributed under the MIT License. See LICENSE.md for details.
--

ALTER TABLE dependencies RENAME TO old_dependencies;

CREATE TABLE dependency_keys(
    id INTEGER PRIMARY KEY,
    savepoint_id_start INT NOT NULL,
    savepoint_id_end INT NOT NULL,
    container_id TEXT NOT NULL,
    configuration_hash TEXT NOT NULL,
    object_id BLOB NOT NULL,
    UNIQUE(savepoint_id_start, savepoint_id_end, container_id,
           configuration_hash, object_id)
) STRICT;

CREATE TABLE dependency_paths(
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE
) STRICT;

CREATE TABLE dependencies(
    key_id INT NOT NULL,
    path_id INT NOT NULL,
    PRIMARY KEY(key_id, path_id)
) WITHOUT ROWID, STRICT;

CREATE INDEX dependency_path_index ON dependencies(path_id);

INSERT INTO dependency_keys(savepoint_id_start, savepoint_id_end, container_id,
                            configuration_hash, object_id)
SELECT DISTINCT savepoint_id_start, savepoint_id_end, container_id,
                configuration_hash, object_id FROM old_dependencies;

INSERT INTO dependency_paths(path) SELECT DISTINCT model_path FROM old_dependencies;

INSERT INTO dependencies
SELECT k.id, p.id FROM old_dependencies AS d
JOIN dependency_keys AS k
  ON k.savepoint_id_start = d.savepoint_id_start
 AND k.savepoint_id_end = d.savepoint_id_end
 AND k.container_id = d.container_id
 AND k.configuration_hash = d.configuration_hash
 AND k.object_id = d.object_id
JOIN dependency_paths AS p ON p.path = d.model_path;

DROP TABLE old_dependencies;
