-- Releases, embargoes and registered identifiers (data publishing and hosted
-- sharing design, docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md,
-- sections 4.2 and 9.2). Additive only: a build before this migration keeps
-- opening a migrated store.
--
-- A release pins revisions through a bookmark (bookmark_entry is append-only
-- and FK-checked already) that sits on an analysis_group of the release's
-- analyses. Its own row changes only in its state columns (release_guard);
-- its samples and interpreted ages are append-only. One name may have several
-- versions; a later one names the one it supersedes.
CREATE TABLE release (uuid uuid PRIMARY KEY,
  name text NOT NULL, title text NOT NULL, description text,
  version int NOT NULL DEFAULT 1, supersedes_uuid uuid REFERENCES release,
  group_uuid uuid NOT NULL REFERENCES analysis_group, bookmark_uuid uuid NOT NULL REFERENCES bookmark,
  state text NOT NULL DEFAULT 'draft' CHECK (state IN ('draft','published','withdrawn')),
  visibility text NOT NULL DEFAULT 'private' CHECK (visibility IN ('private','public')),
  licence text NOT NULL, authors jsonb NOT NULL, publisher text, embargo_until_utc timestamptz,
  doi text, doi_state text CHECK (doi_state IN ('draft','registered','findable')),
  options jsonb NOT NULL, reduction_version text NOT NULL, software_version text NOT NULL,
  manifest_sha256 bytea, created_by_user_uuid uuid NOT NULL REFERENCES app_user,
  client_uuid uuid NOT NULL REFERENCES client, created_utc timestamptz NOT NULL,
  published_utc timestamptz, withdrawn_utc timestamptz, withdrawn_reason text,
  UNIQUE (name, version));
CREATE INDEX release_state_ix ON release (state, visibility);
CREATE TABLE release_sample (release_uuid uuid REFERENCES release, sample_uuid uuid REFERENCES sample,
  PRIMARY KEY (release_uuid, sample_uuid));
CREATE INDEX release_sample_sample_ix ON release_sample (sample_uuid);
CREATE TABLE release_interpreted_age (release_uuid uuid REFERENCES release,
  ia_uuid uuid REFERENCES interpreted_age, revision_uuid uuid NOT NULL REFERENCES revision,
  PRIMARY KEY (release_uuid, ia_uuid));

-- An embargo hides a project, a sample or an analysis from every public
-- surface until a date, or until it is lifted by hand (lifted_utc). Live
-- when lifted_utc IS NULL AND (until_utc IS NULL OR until_utc > now).
-- Mutable, audited by change_log like a catalog row (D6).
CREATE TABLE embargo (uuid uuid PRIMARY KEY,
  scope_type text NOT NULL CHECK (scope_type IN ('project','sample','analysis')),
  scope_uuid uuid NOT NULL, until_utc timestamptz, reason text,
  set_by_user_uuid uuid NOT NULL REFERENCES app_user, created_utc timestamptz NOT NULL,
  lifted_utc timestamptz, lifted_by_user_uuid uuid REFERENCES app_user);
CREATE INDEX embargo_scope_ix ON embargo (scope_type, scope_uuid) WHERE lifted_utc IS NULL;

-- What was minted where, for which entity: the receipt. sample.igsn and
-- release.doi are the catalog's copies.
CREATE TABLE external_id (uuid uuid PRIMARY KEY,
  entity_type text NOT NULL CHECK (entity_type IN ('release','sample')), entity_uuid uuid NOT NULL,
  scheme text NOT NULL CHECK (scheme IN ('doi','igsn')), value text NOT NULL,
  registrar text NOT NULL, state text NOT NULL CHECK (state IN ('draft','registered','findable','in_doubt')),
  request_sha256 bytea NOT NULL, response jsonb, registered_utc timestamptz NOT NULL,
  UNIQUE (scheme, value));
CREATE INDEX external_id_entity_ix ON external_id (entity_type, entity_uuid);

-- The values a published build reported, per release and analysis (spec
-- 9.2): what the hosted views serve. Written by `publish upload` from the
-- release directory's analyses.parquet, never computed in SQL. Both engines
-- have the tables (DDL parity); only PostgreSQL has the views below.
CREATE TABLE pub_analysis_value (release_uuid uuid REFERENCES release, analysis_uuid uuid REFERENCES analysis,
  tag text, included boolean, group_id text, j double precision, j_err double precision,
  ar40_fa double precision, ar40_fa_err double precision, ar39_fa double precision, ar39_fa_err double precision,
  ar38_fa double precision, ar38_fa_err double precision, ar37_fa double precision, ar37_fa_err double precision,
  ar36_fa double precision, ar36_fa_err double precision,
  f double precision, f_err double precision, rad40_pct double precision, k39_fa double precision,
  k39_pct_group double precision, k39_cum_pct double precision, kca double precision, kca_err double precision,
  kcl double precision, kcl_err double precision, iso_39_40 double precision, iso_39_40_err double precision,
  iso_36_40 double precision, iso_36_40_err double precision, iso_corr double precision,
  age_ma double precision, age_err_analytical_ma double precision, age_err_with_j_ma double precision,
  age_note text, PRIMARY KEY (release_uuid, analysis_uuid));
CREATE INDEX pub_analysis_value_analysis_ix ON pub_analysis_value (analysis_uuid);
CREATE TABLE pub_interpreted_age_value (release_uuid uuid REFERENCES release,
  ia_uuid uuid REFERENCES interpreted_age, revision_uuid uuid NOT NULL REFERENCES revision,
  name text NOT NULL, sample_uuid uuid REFERENCES sample, identifier text, age_kind text,
  age_ma double precision, age_err_ma double precision, mswd double precision, n int,
  kca double precision, kca_err double precision, plateau_k39_pct double precision,
  trapped_40_36 double precision, trapped_40_36_err double precision, members jsonb, doc jsonb,
  PRIMARY KEY (release_uuid, ia_uuid));
CREATE INDEX pub_interpreted_age_value_sample_ix ON pub_interpreted_age_value (sample_uuid);

-- ---------- immutability
-- release_sample and release_interpreted_age are never updated, and deleted
-- only with a draft release (IStore::delete_release); a published release's
-- rows are frozen with it.
-- @sqlite skip
CREATE FUNCTION release_member_guard() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF TG_OP = 'UPDATE' THEN
    RAISE EXCEPTION 'pychron: % is append-only', TG_TABLE_NAME;
  END IF;
  IF (SELECT state FROM release WHERE uuid = OLD.release_uuid) IS DISTINCT FROM 'draft' THEN
    RAISE EXCEPTION 'pychron: % of a published release is frozen', TG_TABLE_NAME;
  END IF;
  RETURN OLD;
END $$;
-- @sqlite skip
CREATE TRIGGER release_sample_guard BEFORE UPDATE OR DELETE ON release_sample FOR EACH ROW EXECUTE FUNCTION release_member_guard();
-- @sqlite skip
CREATE TRIGGER release_interpreted_age_guard BEFORE UPDATE OR DELETE ON release_interpreted_age FOR EACH ROW EXECUTE FUNCTION release_member_guard();
-- @sqlite exec CREATE TRIGGER release_sample_no_update BEFORE UPDATE ON release_sample BEGIN SELECT RAISE(ABORT, 'pychron: release_sample is append-only'); END
-- @sqlite exec CREATE TRIGGER release_sample_frozen BEFORE DELETE ON release_sample WHEN (SELECT state FROM release WHERE uuid = OLD.release_uuid) IS NOT 'draft' BEGIN SELECT RAISE(ABORT, 'pychron: release_sample of a published release is frozen'); END
-- @sqlite exec CREATE TRIGGER release_interpreted_age_no_update BEFORE UPDATE ON release_interpreted_age BEGIN SELECT RAISE(ABORT, 'pychron: release_interpreted_age is append-only'); END
-- @sqlite exec CREATE TRIGGER release_interpreted_age_frozen BEFORE DELETE ON release_interpreted_age WHEN (SELECT state FROM release WHERE uuid = OLD.release_uuid) IS NOT 'draft' BEGIN SELECT RAISE(ABORT, 'pychron: release_interpreted_age of a published release is frozen'); END
-- release: deleted only while a draft; updated only in its state columns
-- (spec 4.4). The SQLite triggers are written out below (the guard
-- directive would forbid every DELETE).
-- @sqlite skip
CREATE FUNCTION release_guard() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF TG_OP = 'DELETE' THEN
    IF OLD.state <> 'draft' THEN
      RAISE EXCEPTION 'pychron: a published release is never deleted';
    END IF;
    RETURN OLD;
  END IF;
  IF (to_jsonb(NEW) - ARRAY['state','visibility','doi','doi_state','manifest_sha256','published_utc','withdrawn_utc',
                            'withdrawn_reason'])
     IS DISTINCT FROM
     (to_jsonb(OLD) - ARRAY['state','visibility','doi','doi_state','manifest_sha256','published_utc','withdrawn_utc',
                            'withdrawn_reason']) THEN
    RAISE EXCEPTION 'pychron: release is append-only';
  END IF;
  RETURN NEW;
END $$;
-- @sqlite skip
CREATE TRIGGER release_guard BEFORE UPDATE OR DELETE ON release FOR EACH ROW EXECUTE FUNCTION release_guard();
-- @sqlite exec CREATE TRIGGER release_guard_update BEFORE UPDATE ON release WHEN NEW.uuid IS NOT OLD.uuid OR NEW.name IS NOT OLD.name OR NEW.title IS NOT OLD.title OR NEW.description IS NOT OLD.description OR NEW.version IS NOT OLD.version OR NEW.supersedes_uuid IS NOT OLD.supersedes_uuid OR NEW.group_uuid IS NOT OLD.group_uuid OR NEW.bookmark_uuid IS NOT OLD.bookmark_uuid OR NEW.licence IS NOT OLD.licence OR NEW.authors IS NOT OLD.authors OR NEW.publisher IS NOT OLD.publisher OR NEW.embargo_until_utc IS NOT OLD.embargo_until_utc OR NEW.options IS NOT OLD.options OR NEW.reduction_version IS NOT OLD.reduction_version OR NEW.software_version IS NOT OLD.software_version OR NEW.created_by_user_uuid IS NOT OLD.created_by_user_uuid OR NEW.client_uuid IS NOT OLD.client_uuid OR NEW.created_utc IS NOT OLD.created_utc BEGIN SELECT RAISE(ABORT, 'pychron: release is append-only'); END
-- @sqlite exec CREATE TRIGGER release_no_delete BEFORE DELETE ON release WHEN OLD.state IS NOT 'draft' BEGIN SELECT RAISE(ABORT, 'pychron: a published release is never deleted'); END

-- ---------- hosted views (PostgreSQL only; spec 9.2)
-- Every view applies the visibility rule (spec 4.5): a row is served only
-- through a release that is published and public and past its embargo, and
-- only when nothing of it is under a live embargo. The role that serves them
-- (pychron_api) is granted SELECT on these views alone.
-- @sqlite skip
CREATE VIEW pub_visible_release AS
  SELECT r.* FROM release r
  WHERE r.state = 'published' AND r.visibility = 'public'
    AND (r.embargo_until_utc IS NULL OR r.embargo_until_utc <= now());
-- @sqlite skip
CREATE VIEW pub_live_embargo AS
  SELECT e.scope_type, e.scope_uuid FROM embargo e
  WHERE e.lifted_utc IS NULL AND (e.until_utc IS NULL OR e.until_utc > now());
-- A sample's analyses: the identifier's own sample, else its position's (as browse.cpp finds them).
-- @sqlite skip
CREATE VIEW pub_analysis_sample AS
  SELECT a.uuid AS analysis_uuid, i.sample_uuid FROM analysis a JOIN identifier i ON i.uuid = a.identifier_uuid
  WHERE i.sample_uuid IS NOT NULL
  UNION ALL
  SELECT a.uuid, ip.sample_uuid FROM analysis a JOIN identifier i ON i.uuid = a.identifier_uuid
  JOIN irradiation_position ip ON ip.uuid = i.position_uuid WHERE i.sample_uuid IS NULL AND ip.sample_uuid IS NOT NULL;
-- @sqlite skip
CREATE VIEW pub_visible_sample AS
  SELECT s.* FROM sample s
  WHERE EXISTS (SELECT 1 FROM release_sample rs JOIN pub_visible_release r ON r.uuid = rs.release_uuid
                WHERE rs.sample_uuid = s.uuid)
    AND NOT EXISTS (SELECT 1 FROM pub_live_embargo e WHERE e.scope_type = 'sample' AND e.scope_uuid = s.uuid)
    AND NOT EXISTS (SELECT 1 FROM pub_live_embargo e WHERE e.scope_type = 'project' AND e.scope_uuid = s.project_uuid);
-- @sqlite skip
CREATE VIEW pub_analysis AS
  SELECT v.analysis_uuid, a.runid_text AS runid, i.identifier, a.aliquot, a.increment, s.uuid AS sample_uuid,
         s.name AS sample, p.name AS project, m.name AS material, a.analysis_type, a.experiment_type,
         a.timestamp_utc, ms.name AS mass_spectrometer, ed.name AS extract_device, a.extract_value,
         a.extract_units, a.extract_duration AS extract_duration_s, a.cleanup_duration AS cleanup_duration_s,
         a.weight AS weight_mg, v.tag, v.included, v.group_id, v.j, v.j_err,
         v.ar40_fa, v.ar40_fa_err, v.ar39_fa, v.ar39_fa_err, v.ar38_fa, v.ar38_fa_err, v.ar37_fa, v.ar37_fa_err,
         v.ar36_fa, v.ar36_fa_err, v.f, v.f_err, v.rad40_pct, v.k39_fa, v.k39_pct_group, v.k39_cum_pct,
         v.kca, v.kca_err, v.kcl, v.kcl_err, v.iso_39_40, v.iso_39_40_err, v.iso_36_40, v.iso_36_40_err, v.iso_corr,
         v.age_ma, v.age_err_analytical_ma, v.age_err_with_j_ma, v.age_note, a.record_sha256,
         r.name AS release_name, r.version AS release_version, s.geom
  FROM pub_analysis_value v
  JOIN pub_visible_release r ON r.uuid = v.release_uuid
  JOIN analysis a ON a.uuid = v.analysis_uuid
  JOIN identifier i ON i.uuid = a.identifier_uuid
  JOIN pub_analysis_sample pas ON pas.analysis_uuid = a.uuid
  JOIN pub_visible_sample s ON s.uuid = pas.sample_uuid
  JOIN project p ON p.uuid = s.project_uuid
  JOIN material m ON m.uuid = s.material_uuid
  JOIN mass_spectrometer ms ON ms.uuid = a.mass_spectrometer_uuid
  LEFT JOIN extract_device ed ON ed.uuid = a.extract_device_uuid
  -- the newest published version of each release name that holds the analysis, aggregated once
  JOIN (SELECT r2.name, v2.analysis_uuid, max(r2.version) AS version FROM pub_analysis_value v2
        JOIN pub_visible_release r2 ON r2.uuid = v2.release_uuid GROUP BY r2.name, v2.analysis_uuid) nv
    ON nv.name = r.name AND nv.version = r.version AND nv.analysis_uuid = v.analysis_uuid
  WHERE NOT EXISTS (SELECT 1 FROM pub_live_embargo e WHERE e.scope_type = 'analysis' AND e.scope_uuid = a.uuid);
-- @sqlite skip
CREATE VIEW pub_sample AS
  SELECT s.uuid AS sample_uuid, s.name, s.igsn, p.name AS project, p.uuid AS project_uuid,
         pi.display_name AS principal_investigator, m.name AS material, m.grainsize, s.geom,
         s.elevation AS elevation_m, s.location, s.unit, s.lithology, s.lithology_class, s.lithology_type,
         s.lithology_group, s.approximate_age AS approximate_age_ma, s.note,
         coalesce(c.n_analyses, 0) AS n_analyses, coalesce(c.n_ages, 0) AS n_ages,
         coalesce(ia.n_interpreted_ages, 0) AS n_interpreted_ages, c.first_analysis_utc, c.last_analysis_utc,
         rel.releases, CASE WHEN s.igsn IS NULL THEN NULL ELSE 'https://doi.org/' || s.igsn END AS igsn_doi,
         s.updated_utc
  FROM pub_visible_sample s
  JOIN project p ON p.uuid = s.project_uuid
  LEFT JOIN principal_investigator pi ON pi.uuid = p.pi_uuid
  JOIN material m ON m.uuid = s.material_uuid
  LEFT JOIN (SELECT sample_uuid, count(*) AS n_analyses, count(age_ma) AS n_ages,
                    min(timestamp_utc) AS first_analysis_utc, max(timestamp_utc) AS last_analysis_utc
             FROM pub_analysis GROUP BY sample_uuid) c ON c.sample_uuid = s.uuid
  LEFT JOIN (SELECT v.sample_uuid, count(*) AS n_interpreted_ages FROM pub_interpreted_age_value v
             JOIN pub_visible_release r ON r.uuid = v.release_uuid GROUP BY v.sample_uuid) ia ON ia.sample_uuid = s.uuid
  LEFT JOIN (SELECT rs.sample_uuid,
                    jsonb_agg(jsonb_build_object('name', r.name, 'version', r.version, 'doi', r.doi)
                              ORDER BY r.name, r.version) AS releases
             FROM release_sample rs JOIN pub_visible_release r ON r.uuid = rs.release_uuid
             GROUP BY rs.sample_uuid) rel ON rel.sample_uuid = s.uuid;
-- @sqlite skip
CREATE VIEW pub_interpreted_age AS
  SELECT v.ia_uuid, v.name, v.sample_uuid, v.identifier, v.age_kind, v.age_ma, v.age_err_ma, v.mswd, v.n,
         v.kca, v.kca_err, v.plateau_k39_pct, v.trapped_40_36, v.trapped_40_36_err, v.members, v.revision_uuid,
         v.doc, s.geom, r.name AS release_name, r.version AS release_version
  FROM pub_interpreted_age_value v
  JOIN pub_visible_release r ON r.uuid = v.release_uuid
  JOIN pub_visible_sample s ON s.uuid = v.sample_uuid
  JOIN (SELECT r2.name, v2.ia_uuid, max(r2.version) AS version FROM pub_interpreted_age_value v2
        JOIN pub_visible_release r2 ON r2.uuid = v2.release_uuid GROUP BY r2.name, v2.ia_uuid) nv
    ON nv.name = r.name AND nv.version = r.version AND nv.ia_uuid = v.ia_uuid;
-- @sqlite skip
CREATE VIEW pub_release AS
  SELECT r.uuid AS release_uuid, r.name, r.title, r.description, r.version, r.licence, r.authors, r.publisher,
         r.doi, r.created_utc, r.published_utc, r.software_version, r.reduction_version,
         coalesce(ns.n_samples, 0) AS n_samples, coalesce(na.n_analyses, 0) AS n_analyses,
         CASE WHEN r.doi IS NULL THEN NULL ELSE 'https://doi.org/' || r.doi END AS doi_url, ns.geom
  FROM pub_visible_release r
  LEFT JOIN (SELECT rs.release_uuid, count(*) AS n_samples, public.ST_ConvexHull(public.ST_Collect(s.geom)) AS geom
             FROM release_sample rs JOIN pub_visible_sample s ON s.uuid = rs.sample_uuid
             GROUP BY rs.release_uuid) ns ON ns.release_uuid = r.uuid
  LEFT JOIN (SELECT v.release_uuid, count(*) AS n_analyses FROM pub_analysis_value v GROUP BY v.release_uuid) na
    ON na.release_uuid = r.uuid;
-- One OGC API - Records row per visible sample and per visible release.
-- Materialized for the full-text index; refreshed by `publish upload` and by
-- an embargo change (REFRESH MATERIALIZED VIEW CONCURRENTLY pub_record).
-- @sqlite skip
CREATE MATERIALIZED VIEW pub_record AS
  SELECT 'sample:' || s.sample_uuid::text AS id, 'dataset' AS type,
         s.name || ' (' || s.material || ')' AS title,
         'Sample ' || s.name || ' of project ' || s.project || ': ' || s.n_analyses || ' analyses' AS description,
         s.geom, s.first_analysis_utc AS time_start_utc, s.last_analysis_utc AS time_end_utc,
         ARRAY['40Ar/39Ar', s.material] || (CASE WHEN s.lithology IS NULL THEN ARRAY[]::text[] ELSE ARRAY[s.lithology] END)
           AS keywords,
         (SELECT r->>'licence' FROM (SELECT to_jsonb(vr.*) AS r FROM pub_visible_release vr
                                     JOIN release_sample rs ON rs.release_uuid = vr.uuid
                                     WHERE rs.sample_uuid = s.sample_uuid ORDER BY vr.published_utc DESC LIMIT 1) x)
           AS license,
         s.updated_utc AS created_utc, s.updated_utc,
         CASE WHEN s.igsn IS NULL THEN '[]'::jsonb
              ELSE jsonb_build_array(jsonb_build_object('scheme', 'igsn', 'value', s.igsn)) END AS external_ids,
         coalesce(s.releases, '[]'::jsonb) AS links,
         to_tsvector('english', s.name || ' ' || s.project || ' ' || s.material || ' ' || coalesce(s.lithology, ''))
           AS search
  FROM pub_sample s
  UNION ALL
  SELECT 'release:' || r.release_uuid::text, 'dataset', r.title, coalesce(r.description, ''), r.geom,
         r.published_utc, r.published_utc, ARRAY['40Ar/39Ar', 'release', r.name], r.licence,
         r.created_utc, coalesce(r.published_utc, r.created_utc),
         CASE WHEN r.doi IS NULL THEN '[]'::jsonb
              ELSE jsonb_build_array(jsonb_build_object('scheme', 'doi', 'value', r.doi)) END,
         jsonb_build_array(jsonb_build_object('name', r.name, 'version', r.version, 'doi', r.doi)),
         to_tsvector('english', r.title || ' ' || coalesce(r.description, '') || ' ' || r.name)
  FROM pub_release r;
-- @sqlite skip
CREATE UNIQUE INDEX pub_record_id_ux ON pub_record (id);
-- @sqlite skip
CREATE INDEX pub_record_search_gin ON pub_record USING GIN (search);
