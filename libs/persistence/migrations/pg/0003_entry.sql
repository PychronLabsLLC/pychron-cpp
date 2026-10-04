-- Sample and package entry (2026-10-04-sample-irradiation-entry-design.md, section 5.5).
-- The package kind: an irradiation package (chronology, productions, flux) or a
-- plain package (positions and samples only).
ALTER TABLE irradiation ADD COLUMN kind text NOT NULL DEFAULT 'irradiation'
  CHECK (kind IN ('irradiation','package'));
-- Case-insensitive sample and project search.
CREATE INDEX sample_name_lower_ix ON sample (lower(name));
CREATE INDEX project_name_lower_ix ON project (lower(name));
