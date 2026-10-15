-- A principal investigator is a person or an organisation (PI spec,
-- docs/superpowers/specs/2026-10-13-pi-person-organisation-design.md, section 4).
-- A person's family and given name are kept in full; an ORCID is optional and
-- unique among the rows that have one; a ROR names the affiliation. Two PIs
-- may have the same name: UNIQUE (last_name, first_initial) goes, and a PI is
-- known by its uuid. Every uuid is kept, so projects and repositories point at
-- the same PIs as before.
--
-- Shown (display_name): a person "Given Family" ("Jake Ross", "J Ross"), or
-- the family name alone without a given name; an organisation its name.
--
-- PostgreSQL changes the table in place. SQLite cannot drop a column a UNIQUE
-- names nor add a stored generated column, so the table is rebuilt. What that
-- relies on: the store opens every SQLite connection with foreign keys on
-- (check_sqlite, store.cpp) and a migration is one transaction (migrate.cpp),
-- in which PRAGMA foreign_keys cannot change. The REFERENCES of project and
-- repository are immediate; defer_foreign_keys makes them deferred until this
-- transaction ends. Dropping the old table deletes its rows, which leaves each
-- project and repository with a PI pointing at nothing: a deferred violation
-- each. Inserting the same uuids into the new table resolves every one of
-- them, so the commit finds none. A PI whose row did not reach the new table
-- would fail the commit and leave the store as it was. ALTER TABLE ... RENAME
-- is not used: with foreign keys on, SQLite rewrites the references of other
-- tables to follow a renamed table.
-- @sqlite skip
ALTER TABLE principal_investigator DROP COLUMN display_name;
-- @sqlite skip
ALTER TABLE principal_investigator DROP CONSTRAINT principal_investigator_last_name_first_initial_key;
-- @sqlite skip
ALTER TABLE principal_investigator RENAME COLUMN last_name TO family_name;
-- @sqlite skip
ALTER TABLE principal_investigator RENAME COLUMN first_initial TO given_name;
-- @sqlite skip
ALTER TABLE principal_investigator ADD COLUMN kind text NOT NULL DEFAULT 'person'
  CHECK (kind IN ('person','organisation'));
-- @sqlite skip
ALTER TABLE principal_investigator ADD COLUMN orcid text;
-- @sqlite skip
ALTER TABLE principal_investigator ADD COLUMN ror text;
-- @sqlite skip
ALTER TABLE principal_investigator ADD CONSTRAINT principal_investigator_organisation_check
  CHECK (kind = 'person' OR (orcid IS NULL AND given_name = ''));
-- @sqlite skip
ALTER TABLE principal_investigator ADD COLUMN display_name text GENERATED ALWAYS AS
  (CASE WHEN kind = 'organisation' OR given_name = '' THEN family_name ELSE given_name || ' ' || family_name END) STORED;
-- @sqlite exec PRAGMA defer_foreign_keys = ON
-- @sqlite exec CREATE TABLE principal_investigator_0006 AS SELECT uuid, last_name, first_initial, affiliation, email, created_utc FROM principal_investigator
-- @sqlite exec DROP TABLE principal_investigator
-- @sqlite exec CREATE TABLE principal_investigator ( uuid TEXT NOT NULL PRIMARY KEY CHECK (length(uuid) = 36), kind TEXT NOT NULL DEFAULT 'person' CHECK (kind IN ('person','organisation')), family_name TEXT NOT NULL, given_name TEXT NOT NULL DEFAULT '', orcid TEXT, ror TEXT, affiliation TEXT, email TEXT, display_name TEXT GENERATED ALWAYS AS (CASE WHEN kind = 'organisation' OR given_name = '' THEN family_name ELSE given_name || ' ' || family_name END) STORED, created_utc TEXT NOT NULL CHECK (created_utc LIKE '____-__-__T__:__:__%Z'), CHECK (kind = 'person' OR (orcid IS NULL AND given_name = '')) ) STRICT
-- @sqlite exec INSERT INTO principal_investigator (uuid, kind, family_name, given_name, affiliation, email, created_utc) SELECT uuid, 'person', last_name, first_initial, affiliation, email, created_utc FROM principal_investigator_0006
-- @sqlite exec DROP TABLE principal_investigator_0006
CREATE UNIQUE INDEX principal_investigator_orcid ON principal_investigator (orcid) WHERE orcid IS NOT NULL;
