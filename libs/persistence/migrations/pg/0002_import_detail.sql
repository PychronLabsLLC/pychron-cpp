-- Import provenance carries optional per-entity detail (legacy ingestion, task 3).
ALTER TABLE import_provenance ADD COLUMN detail jsonb;
