-- 0001_init.sql (PostgreSQL >= 14). DVC schema spec 2026-10-01, Appendix A (section 11.1).
--
-- This file is the single source of the schema. migrations/sqlite/0001_init.sql is generated
-- from it by tools/ddl_sqlite.py; regenerate after any edit. Lines starting with '-- @sqlite'
-- are directives for that generator.
CREATE TABLE schema_version (version int PRIMARY KEY, description text NOT NULL, checksum bytea NOT NULL,
  applied_utc timestamptz NOT NULL DEFAULT now());
CREATE TABLE schema_compat (key text PRIMARY KEY, value text NOT NULL);
  -- min_writer_version, min_reader_version, payload_versions_accepted

-- ---------- people / clients / instruments
CREATE TABLE principal_investigator (uuid uuid PRIMARY KEY, last_name text NOT NULL,
  first_initial text NOT NULL DEFAULT '', affiliation text, email text,
  display_name text GENERATED ALWAYS AS
    (CASE WHEN first_initial = '' THEN last_name ELSE last_name || ', ' || first_initial END) STORED,
  created_utc timestamptz NOT NULL, UNIQUE (last_name, first_initial));
CREATE TABLE app_user (uuid uuid PRIMARY KEY, name text NOT NULL UNIQUE, email text, affiliation text,
  category text, created_utc timestamptz NOT NULL);
CREATE TABLE mass_spectrometer (uuid uuid PRIMARY KEY, name text NOT NULL UNIQUE, kind text, code text UNIQUE,
  created_utc timestamptz NOT NULL);
CREATE TABLE extract_device (uuid uuid PRIMARY KEY, name text NOT NULL UNIQUE, code text,
  created_utc timestamptz NOT NULL);
CREATE TABLE client (uuid uuid PRIMARY KEY, hostname text NOT NULL,
  role text NOT NULL CHECK (role IN ('acquisition','reduction','viewer','publisher','importer','admin')),
  mass_spectrometer_uuid uuid REFERENCES mass_spectrometer, software_version text,
  created_utc timestamptz NOT NULL, UNIQUE (hostname, role));

-- ---------- projects / samples
CREATE TABLE project (uuid uuid PRIMARY KEY, name text NOT NULL, pi_uuid uuid REFERENCES principal_investigator,
  checkin_date date, comment text, lab_contact text, institution text, created_utc timestamptz NOT NULL,
  UNIQUE (name, pi_uuid));
CREATE TABLE material (uuid uuid PRIMARY KEY, name text NOT NULL, grainsize text NOT NULL DEFAULT '',
  created_utc timestamptz NOT NULL, UNIQUE (name, grainsize));
CREATE TABLE sample (uuid uuid PRIMARY KEY, name text NOT NULL, project_uuid uuid NOT NULL REFERENCES project,
  material_uuid uuid NOT NULL REFERENCES material, note text, igsn text, lat double precision,
  lon double precision, elevation double precision, storage_location text, location text, unit text,
  lithology text, lithology_class text, lithology_type text, lithology_group text,
  approximate_age double precision, created_utc timestamptz NOT NULL, updated_utc timestamptz NOT NULL,
  UNIQUE (name, project_uuid, material_uuid));
CREATE INDEX sample_igsn_ix ON sample (igsn);

-- ---------- reference objects (declared early: levels and loads reference holders)
CREATE TABLE ref_object (uuid uuid PRIMARY KEY,
  ref_type text NOT NULL CHECK (ref_type IN ('flux_position','level_geometry','production','level_production',
    'chronology','gains','sensitivity','irradiation_holder','load_holder','script','document')),
  key text NOT NULL, irradiation_uuid uuid, level_uuid uuid, position_uuid uuid,
  mass_spectrometer_uuid uuid REFERENCES mass_spectrometer, created_utc timestamptz NOT NULL,
  UNIQUE (ref_type, key));

-- ---------- irradiations / identifiers
CREATE TABLE irradiation (uuid uuid PRIMARY KEY, name text NOT NULL UNIQUE, created_utc timestamptz NOT NULL);
CREATE TABLE level (uuid uuid PRIMARY KEY, irradiation_uuid uuid NOT NULL REFERENCES irradiation,
  name text NOT NULL, holder_ref_uuid uuid REFERENCES ref_object, z double precision, note text,
  created_utc timestamptz NOT NULL, UNIQUE (irradiation_uuid, name));
CREATE TABLE irradiation_position (uuid uuid PRIMARY KEY, level_uuid uuid NOT NULL REFERENCES level,
  position int NOT NULL, sample_uuid uuid REFERENCES sample, weight double precision, packet text, note text,
  created_utc timestamptz NOT NULL, UNIQUE (level_uuid, position));
ALTER TABLE ref_object
  ADD FOREIGN KEY (irradiation_uuid) REFERENCES irradiation,
  ADD FOREIGN KEY (level_uuid) REFERENCES level,
  ADD FOREIGN KEY (position_uuid) REFERENCES irradiation_position;
CREATE TABLE identifier (uuid uuid PRIMARY KEY, identifier text NOT NULL UNIQUE,
  kind text NOT NULL CHECK (kind IN ('unknown','special')),
  position_uuid uuid UNIQUE REFERENCES irradiation_position, sample_uuid uuid REFERENCES sample,
  analysis_type text, mass_spectrometer_uuid uuid REFERENCES mass_spectrometer,
  extract_device_uuid uuid REFERENCES extract_device, created_utc timestamptz NOT NULL,
  CHECK (kind = 'unknown' OR (position_uuid IS NULL AND analysis_type IS NOT NULL)));
CREATE TABLE identifier_counter (scope text PRIMARY KEY, last_value bigint NOT NULL);

-- ---------- loads
CREATE TABLE load (uuid uuid PRIMARY KEY, name text NOT NULL UNIQUE, holder_ref_uuid uuid REFERENCES ref_object,
  holder_ref_revision_uuid uuid, created_by_user_uuid uuid REFERENCES app_user,
  archived boolean NOT NULL DEFAULT false, created_utc timestamptz NOT NULL);
CREATE TABLE load_position (uuid uuid PRIMARY KEY, load_uuid uuid NOT NULL REFERENCES load,
  position int NOT NULL, identifier_uuid uuid NOT NULL REFERENCES identifier, weight double precision,
  nxtals int, note text, created_utc timestamptz NOT NULL, UNIQUE (load_uuid, position, identifier_uuid));

-- ---------- content-addressed bytes
CREATE TABLE signal_blob (sha256 bytea PRIMARY KEY CHECK (length(sha256) = 32), codec text NOT NULL,
  byte_len int NOT NULL, n_points int, bytes bytea NOT NULL, created_utc timestamptz NOT NULL,
  CHECK (sha256 = sha256(convert_to(codec, 'UTF8') || '\x00'::bytea || bytes)));
CREATE TABLE script_text (sha256 bytea PRIMARY KEY, body text NOT NULL, created_utc timestamptz NOT NULL);
CREATE TABLE spectrometer_snapshot (sha256 bytea PRIMARY KEY, legacy_sha1 text, spectrometer jsonb, gains jsonb,
  deflections jsonb, settings jsonb, created_utc timestamptz NOT NULL);

-- ---------- changesets (declared before analysis: analysis.ingest_changeset_uuid)
CREATE TABLE import_source (uuid uuid PRIMARY KEY,
  kind text NOT NULL CHECK (kind IN ('project_repo','meta_repo','legacy_db')), url_or_path text NOT NULL,
  branch text, head_commit_sha text, progress_commit_sha text, commits_total int, commits_done int,
  importer_version text NOT NULL, lab_time_zone text NOT NULL,
  started_utc timestamptz NOT NULL, finished_utc timestamptz, status text NOT NULL);
CREATE TABLE changeset (uuid uuid PRIMARY KEY,
  kind text NOT NULL CHECK (kind IN ('collection','reduction','rollback','bookmark_restore','import',
    'reference','admin')),
  author_user_uuid uuid NOT NULL REFERENCES app_user, client_uuid uuid NOT NULL REFERENCES client,
  message text NOT NULL, created_utc timestamptz NOT NULL, import_source_uuid uuid REFERENCES import_source);

-- ---------- analyses
CREATE TABLE experiment_queue (uuid uuid PRIMARY KEY, name text NOT NULL,
  mass_spectrometer_uuid uuid REFERENCES mass_spectrometer, creator_user_uuid uuid REFERENCES app_user,
  text_blob_sha bytea, schema_version int, created_utc timestamptz NOT NULL);
CREATE TABLE analysis (           -- runid_text is maintained by the access layer with the identity columns
  uuid uuid PRIMARY KEY, identifier_uuid uuid NOT NULL REFERENCES identifier, aliquot int NOT NULL,
  increment int NOT NULL DEFAULT -1, provisional boolean NOT NULL DEFAULT false, runid_text text NOT NULL,
  analysis_type text NOT NULL, experiment_type text, timestamp_utc timestamptz NOT NULL, time_zero_utc timestamptz,
  mass_spectrometer_uuid uuid NOT NULL REFERENCES mass_spectrometer, extract_device_uuid uuid REFERENCES extract_device,
  extract_value double precision, extract_units text, extract_duration double precision,
  cleanup_duration double precision, pre_cleanup double precision, post_cleanup double precision,
  cryo_temperature double precision, weight double precision, beam_diameter double precision, pattern text,
  ramp_duration double precision, ramp_rate double precision, light_value double precision, tray text,
  load_uuid uuid REFERENCES load, load_holder text,
  measurement_script_sha bytea REFERENCES script_text, extraction_script_sha bytea REFERENCES script_text,
  post_eq_script_sha bytea REFERENCES script_text, post_meas_script_sha bytea REFERENCES script_text,
  hops_script_sha bytea REFERENCES script_text, spectrometer_snapshot_sha bytea REFERENCES spectrometer_snapshot,
  queue_uuid uuid REFERENCES experiment_queue, run_index int, laboratory text, instrument_name text,
  analyst_user_uuid uuid REFERENCES app_user, acquisition_client_uuid uuid REFERENCES client,
  record_sha256 bytea, record_schema_version int NOT NULL,
  signals_state text NOT NULL DEFAULT 'pending' CHECK (signals_state IN ('pending','complete')),
  ingest_changeset_uuid uuid NOT NULL REFERENCES changeset, created_utc timestamptz NOT NULL,
  UNIQUE (identifier_uuid, aliquot, increment));
CREATE INDEX analysis_ts_ix ON analysis (timestamp_utc);
CREATE INDEX analysis_ms_type_ts_ix ON analysis (mass_spectrometer_uuid, analysis_type, timestamp_utc);
CREATE INDEX analysis_load_ix ON analysis (load_uuid);
CREATE INDEX analysis_runid_ix ON analysis (runid_text);
CREATE TABLE analysis_meta (analysis_uuid uuid PRIMARY KEY REFERENCES analysis, source jsonb,
  environmental jsonb, conditionals jsonb, tripped_conditional jsonb, whiff_result jsonb,
  intensity_scalar double precision, baseline_modifiers jsonb, arar_mapping jsonb, extraction_context jsonb,
  pid jsonb, snapshots jsonb, videos jsonb, grain_polygons jsonb, pipette_counts jsonb, software jsonb,
  queue_names jsonb, legacy jsonb);
CREATE TABLE analysis_isotope (analysis_uuid uuid REFERENCES analysis, isotope text, detector text NOT NULL,
  units text, detector_serial text, classification text, classification_probability double precision,
  PRIMARY KEY (analysis_uuid, isotope));
CREATE TABLE analysis_detector (analysis_uuid uuid REFERENCES analysis, detector text,
  deflection double precision, gain_used double precision, PRIMARY KEY (analysis_uuid, detector));
CREATE TABLE measured_position (uuid uuid PRIMARY KEY, analysis_uuid uuid NOT NULL REFERENCES analysis,
  load_uuid uuid REFERENCES load, position int, x double precision, y double precision, z double precision,
  is_degas boolean NOT NULL DEFAULT false);
CREATE INDEX measured_position_an_ix ON measured_position (analysis_uuid);
CREATE INDEX measured_position_load_ix ON measured_position (load_uuid, position);
CREATE TABLE peak_center (analysis_uuid uuid REFERENCES analysis, detector text, reference_detector text,
  reference_isotope text, interpolation text, low_dac double precision, center_dac double precision,
  high_dac double precision, low_signal double precision, center_signal double precision,
  high_signal double precision, resolution double precision, low_resolving_power double precision,
  high_resolving_power double precision, points_blob_sha bytea, PRIMARY KEY (analysis_uuid, detector));
CREATE TABLE monitor_check (analysis_uuid uuid REFERENCES analysis, ordinal int, name text, parameter text,
  criterion text, comparator text, tripped boolean, data_blob_sha bytea, PRIMARY KEY (analysis_uuid, ordinal));
CREATE TABLE analysis_artifact (analysis_uuid uuid REFERENCES analysis, name text,
  kind text NOT NULL CHECK (kind IN ('log','snapshot','video','stream','other')), blob_sha bytea, url text,
  CHECK (blob_sha IS NOT NULL OR url IS NOT NULL), PRIMARY KEY (analysis_uuid, name));

-- ---------- groups / repositories
CREATE TABLE analysis_group (uuid uuid PRIMARY KEY, name text NOT NULL, project_uuid uuid REFERENCES project,
  created_by_user_uuid uuid REFERENCES app_user, created_utc timestamptz NOT NULL);
CREATE TABLE analysis_group_member (group_uuid uuid REFERENCES analysis_group,
  analysis_uuid uuid REFERENCES analysis, PRIMARY KEY (group_uuid, analysis_uuid));
CREATE TABLE repository (uuid uuid PRIMARY KEY, name text NOT NULL UNIQUE,
  pi_uuid uuid REFERENCES principal_investigator, created_utc timestamptz NOT NULL);
CREATE TABLE repository_member (repository_uuid uuid REFERENCES repository,
  analysis_uuid uuid REFERENCES analysis, added_changeset_uuid uuid REFERENCES changeset,
  PRIMARY KEY (repository_uuid, analysis_uuid));
CREATE INDEX repository_member_an_ix ON repository_member (analysis_uuid);

-- ---------- interpreted ages
CREATE TABLE interpreted_age (uuid uuid PRIMARY KEY, name text NOT NULL,
  identifier_uuid uuid REFERENCES identifier, repository_uuid uuid REFERENCES repository,
  created_utc timestamptz NOT NULL);

-- ---------- revisions
CREATE TABLE revision (uuid uuid PRIMARY KEY, changeset_uuid uuid NOT NULL REFERENCES changeset,
  subject_type text NOT NULL CHECK (subject_type IN ('analysis','ref','ia')), subject_uuid uuid NOT NULL,
  analysis_uuid uuid REFERENCES analysis, ref_object_uuid uuid REFERENCES ref_object,
  ia_uuid uuid REFERENCES interpreted_age,
  kind text NOT NULL CHECK (kind IN ('signals','intercepts','baselines','blanks','icfactors','tags',
    'annotation','refpins','identity','cosmogenic','interpreted_age','value')),
  parent_uuid uuid, created_utc timestamptz NOT NULL,
  CHECK ((subject_type = 'analysis' AND analysis_uuid = subject_uuid AND ref_object_uuid IS NULL AND ia_uuid IS NULL)
      OR (subject_type = 'ref' AND ref_object_uuid = subject_uuid AND analysis_uuid IS NULL AND ia_uuid IS NULL
          AND kind = 'value')
      OR (subject_type = 'ia' AND ia_uuid = subject_uuid AND analysis_uuid IS NULL AND ref_object_uuid IS NULL
          AND kind = 'interpreted_age')),
  UNIQUE (uuid, subject_uuid, kind),
  FOREIGN KEY (parent_uuid, subject_uuid, kind) REFERENCES revision (uuid, subject_uuid, kind));
CREATE INDEX revision_subject_ix ON revision (subject_uuid, kind, created_utc);
CREATE INDEX revision_changeset_ix ON revision (changeset_uuid);
CREATE TABLE head (subject_uuid uuid NOT NULL, kind text NOT NULL, revision_uuid uuid NOT NULL,
  head_version int NOT NULL DEFAULT 1, PRIMARY KEY (subject_uuid, kind),
  FOREIGN KEY (revision_uuid, subject_uuid, kind) REFERENCES revision (uuid, subject_uuid, kind));
CREATE TABLE head_move (uuid uuid PRIMARY KEY, changeset_uuid uuid NOT NULL REFERENCES changeset,
  subject_uuid uuid NOT NULL, kind text NOT NULL, from_revision_uuid uuid REFERENCES revision,
  to_revision_uuid uuid NOT NULL REFERENCES revision,
  reason text NOT NULL CHECK (reason IN ('commit','rollback','bookmark_restore','collection_restore','ingest')));
CREATE INDEX head_move_subject_ix ON head_move (subject_uuid, kind);
CREATE TABLE bookmark (uuid uuid PRIMARY KEY, name text NOT NULL, repository_uuid uuid REFERENCES repository,
  group_uuid uuid REFERENCES analysis_group, message text, author_user_uuid uuid REFERENCES app_user,
  created_utc timestamptz NOT NULL, CHECK (repository_uuid IS NOT NULL OR group_uuid IS NOT NULL));
CREATE TABLE bookmark_entry (bookmark_uuid uuid REFERENCES bookmark, subject_uuid uuid, kind text,
  revision_uuid uuid NOT NULL, PRIMARY KEY (bookmark_uuid, subject_uuid, kind),
  FOREIGN KEY (revision_uuid, subject_uuid, kind) REFERENCES revision (uuid, subject_uuid, kind));

-- ---------- analysis payloads (all PK-led by revision_uuid; insert-only)
CREATE TABLE intercept_value (revision_uuid uuid REFERENCES revision, isotope text, detector text,
  value double precision, error double precision, fit text, error_type text, n int, fn int,
  include_baseline_error boolean, filter_outliers jsonb, user_excluded jsonb, outlier_excluded jsonb,
  reviewed boolean NOT NULL DEFAULT false, use_manual_value boolean NOT NULL DEFAULT false,
  manual_value double precision, use_manual_error boolean NOT NULL DEFAULT false, manual_error double precision,
  extra jsonb, PRIMARY KEY (revision_uuid, isotope));
CREATE TABLE baseline_value (revision_uuid uuid REFERENCES revision, detector text, value double precision,
  error double precision, fit text, error_type text, n int, fn int, filter_outliers jsonb, user_excluded jsonb,
  modifier_value double precision, modifier_error double precision, reviewed boolean NOT NULL DEFAULT false,
  use_manual_value boolean NOT NULL DEFAULT false, manual_value double precision,
  use_manual_error boolean NOT NULL DEFAULT false, manual_error double precision, extra jsonb,
  PRIMARY KEY (revision_uuid, detector));
CREATE TABLE blank_value (revision_uuid uuid REFERENCES revision, isotope text, value double precision,
  error double precision, fit text, error_type text, reviewed boolean NOT NULL DEFAULT false,
  use_manual_value boolean NOT NULL DEFAULT false, manual_value double precision,
  use_manual_error boolean NOT NULL DEFAULT false, manual_error double precision, extra jsonb,
  PRIMARY KEY (revision_uuid, isotope));
CREATE TABLE blank_reference (revision_uuid uuid, isotope text, ordinal int,
  ref_analysis_uuid uuid REFERENCES analysis, record_id text, exclude boolean NOT NULL DEFAULT false,
  PRIMARY KEY (revision_uuid, isotope, ordinal), FOREIGN KEY (revision_uuid, isotope) REFERENCES blank_value);
CREATE TABLE icfactor_value (revision_uuid uuid REFERENCES revision, detector text, value double precision,
  error double precision, fit text, reviewed boolean NOT NULL DEFAULT false, reference_detector text,
  standard_ratio double precision, discrimination boolean NOT NULL DEFAULT false,
  source_correction boolean NOT NULL DEFAULT false, reference_data jsonb,
  use_manual_value boolean NOT NULL DEFAULT false, manual_value double precision,
  use_manual_error boolean NOT NULL DEFAULT false, manual_error double precision, extra jsonb,
  PRIMARY KEY (revision_uuid, detector));
CREATE TABLE icfactor_reference (revision_uuid uuid, detector text, ordinal int,
  ref_analysis_uuid uuid REFERENCES analysis, record_id text, exclude boolean NOT NULL DEFAULT false,
  PRIMARY KEY (revision_uuid, detector, ordinal),
  FOREIGN KEY (revision_uuid, detector) REFERENCES icfactor_value);
CREATE TABLE signal_ref (
  revision_uuid uuid REFERENCES revision,
  series_kind text CHECK (series_kind IN ('signal','baseline','sniff','whiff')),
  series_key text, detector text NOT NULL,
  blob_sha bytea NOT NULL,                 -- no FK: blobs may arrive later (section 7.3)
  n_points int, start_index int, end_index int,
  PRIMARY KEY (revision_uuid, series_kind, series_key));
CREATE INDEX signal_ref_blob_ix ON signal_ref (blob_sha);
CREATE TABLE tag_value (revision_uuid uuid PRIMARY KEY REFERENCES revision, name text NOT NULL, note text,
  subgroup jsonb);
CREATE TABLE annotation_value (revision_uuid uuid PRIMARY KEY REFERENCES revision, comment text);
CREATE TABLE refpin_value (revision_uuid uuid REFERENCES revision, ref_object_uuid uuid REFERENCES ref_object,
  ref_revision_uuid uuid NOT NULL REFERENCES revision, PRIMARY KEY (revision_uuid, ref_object_uuid));
CREATE TABLE identity_value (revision_uuid uuid PRIMARY KEY REFERENCES revision,
  identifier_uuid uuid NOT NULL REFERENCES identifier, aliquot int NOT NULL, increment int NOT NULL,
  reason text NOT NULL);
CREATE TABLE cosmogenic_value (revision_uuid uuid PRIMARY KEY REFERENCES revision, doc jsonb NOT NULL);
CREATE TABLE ia_value (revision_uuid uuid PRIMARY KEY REFERENCES revision, age double precision,
  age_err double precision, age_kind text, kca double precision, kca_err double precision,
  mswd double precision, nanalyses int, doc jsonb NOT NULL);
CREATE TABLE ia_member (revision_uuid uuid REFERENCES ia_value, analysis_uuid uuid REFERENCES analysis,
  record_id text, plateau_step boolean, tag text, PRIMARY KEY (revision_uuid, analysis_uuid));

-- ---------- reference payloads
CREATE TABLE flux_value (revision_uuid uuid PRIMARY KEY REFERENCES revision, j double precision,
  j_err double precision, mean_j double precision, mean_j_err double precision, mean_j_mswd double precision,
  position_jerr double precision, lambda_k_total double precision, lambda_k_total_err double precision,
  monitor_name text, monitor_material text, monitor_age double precision, monitor_age_err double precision,
  options jsonb, extra jsonb);
CREATE TABLE flux_value_analysis (revision_uuid uuid REFERENCES flux_value, analysis_uuid uuid,
  record_id text NOT NULL, is_omitted boolean NOT NULL, PRIMARY KEY (revision_uuid, record_id));
CREATE TABLE level_z_value (revision_uuid uuid PRIMARY KEY REFERENCES revision, z double precision);
CREATE TABLE production_meta (revision_uuid uuid PRIMARY KEY REFERENCES revision, reactor text, note text);
CREATE TABLE production_value (revision_uuid uuid REFERENCES production_meta, key text,
  value double precision NOT NULL, error double precision NOT NULL, PRIMARY KEY (revision_uuid, key));
CREATE TABLE level_production_value (revision_uuid uuid PRIMARY KEY REFERENCES revision,
  production_ref_uuid uuid NOT NULL REFERENCES ref_object, note text);
CREATE TABLE chronology_dose (revision_uuid uuid REFERENCES revision, ordinal int,
  power double precision NOT NULL, start_utc timestamptz NOT NULL, end_utc timestamptz NOT NULL,
  PRIMARY KEY (revision_uuid, ordinal));
CREATE TABLE detector_gain (revision_uuid uuid REFERENCES revision, detector text,
  gain double precision NOT NULL, PRIMARY KEY (revision_uuid, detector));
CREATE TABLE sensitivity_value (revision_uuid uuid PRIMARY KEY REFERENCES revision,
  sensitivity double precision NOT NULL, create_date_utc timestamptz, extra jsonb);
CREATE TABLE holder_meta (revision_uuid uuid PRIMARY KEY REFERENCES revision, shape text,
  radius double precision, has_hole_numbers boolean NOT NULL DEFAULT false);
CREATE TABLE holder_hole (revision_uuid uuid REFERENCES holder_meta, ordinal int, hole_id text NOT NULL,
  x double precision NOT NULL, y double precision NOT NULL, radius double precision,
  PRIMARY KEY (revision_uuid, ordinal));
CREATE TABLE script_version (revision_uuid uuid PRIMARY KEY REFERENCES revision,
  script_sha bytea NOT NULL REFERENCES script_text);
CREATE TABLE ref_document (revision_uuid uuid PRIMARY KEY REFERENCES revision, content_text text,
  content_json jsonb);
ALTER TABLE load ADD FOREIGN KEY (holder_ref_revision_uuid) REFERENCES revision;

-- ---------- derived cache (the only value table allowing DELETE)
CREATE TABLE derived_value (analysis_uuid uuid REFERENCES analysis, fingerprint bytea, name text,
  value double precision, error double precision, units text, reduction_version text NOT NULL,
  computed_utc timestamptz NOT NULL, PRIMARY KEY (analysis_uuid, fingerprint, name));
CREATE INDEX derived_value_name_ix ON derived_value (name, value);

-- ---------- coordination
CREATE TABLE aliquot_lease (identifier_uuid uuid REFERENCES identifier, aliquot int,
  client_uuid uuid NOT NULL REFERENCES client, queue_uuid uuid,
  state text NOT NULL CHECK (state IN ('leased','used','released','expired')), leased_utc timestamptz NOT NULL,
  expires_utc timestamptz, PRIMARY KEY (identifier_uuid, aliquot));
CREATE INDEX aliquot_lease_client_ix ON aliquot_lease (client_uuid, state);
CREATE TABLE ingest_receipt (item_uuid uuid PRIMARY KEY, client_uuid uuid NOT NULL REFERENCES client,
  kind text NOT NULL, payload_sha256 bytea NOT NULL, change_seq bigint NOT NULL,
  ingested_utc timestamptz NOT NULL DEFAULT now());

-- ---------- change cursor
CREATE TABLE change_counter (id int PRIMARY KEY CHECK (id = 1), seq bigint NOT NULL);
INSERT INTO change_counter VALUES (1, 0);
CREATE TABLE change_log (change_seq bigint PRIMARY KEY, committed_utc timestamptz NOT NULL DEFAULT now(),
  changeset_uuid uuid REFERENCES changeset, client_uuid uuid NOT NULL REFERENCES client,
  kind text NOT NULL CHECK (kind IN ('changeset','ingest','blob_complete','catalog','lease')));
CREATE TABLE change_entity (change_seq bigint REFERENCES change_log, entity_type text, entity_uuid uuid,
  op text NOT NULL DEFAULT 'upsert', detail jsonb,  -- catalog edits: {"field": [old, new], ...} (D6)
  PRIMARY KEY (change_seq, entity_type, entity_uuid));
CREATE TABLE sync_conflict (uuid uuid PRIMARY KEY, item_uuid uuid NOT NULL, client_uuid uuid NOT NULL REFERENCES client,
  author_user_uuid uuid REFERENCES app_user, changeset_uuid uuid NOT NULL,  -- never committed; no FK
  kind text NOT NULL CHECK (kind IN ('cas','provisional_renumber')), heads jsonb NOT NULL,
  winning_changeset_uuid uuid REFERENCES changeset, detail jsonb, created_utc timestamptz NOT NULL DEFAULT now(),
  resolution text NOT NULL DEFAULT 'pending' CHECK (resolution IN ('pending','reapplied','discarded','accepted')),
  resolved_utc timestamptz);
CREATE INDEX change_entity_ix ON change_entity (entity_type, entity_uuid);

-- ---------- publisher / importer
CREATE TABLE publish_target (uuid uuid PRIMARY KEY,
  kind text NOT NULL CHECK (kind IN ('project_repo','meta_repo')), repository_uuid uuid REFERENCES repository,
  remote_url text, branch text NOT NULL DEFAULT 'master', layout jsonb NOT NULL, batch_policy text NOT NULL,
  enabled boolean NOT NULL DEFAULT true);
CREATE TABLE publish_state (target_uuid uuid PRIMARY KEY REFERENCES publish_target,
  published_through_seq bigint NOT NULL DEFAULT 0, pushed_through_seq bigint NOT NULL DEFAULT 0,
  last_commit_sha text, last_push_utc timestamptz, state text NOT NULL, attempts int NOT NULL DEFAULT 0,
  next_attempt_utc timestamptz, last_error text);
CREATE TABLE publish_commit (target_uuid uuid REFERENCES publish_target, commit_sha text,
  first_seq bigint NOT NULL, last_seq bigint NOT NULL, changeset_uuids jsonb NOT NULL,
  published_utc timestamptz NOT NULL, PRIMARY KEY (target_uuid, commit_sha));
CREATE TABLE published_file (target_uuid uuid REFERENCES publish_target, path text, subject_uuid uuid,
  kind text, revision_uuid uuid REFERENCES revision, file_sha256 bytea NOT NULL, commit_sha text NOT NULL,
  PRIMARY KEY (target_uuid, path));
CREATE TABLE import_provenance (entity_type text, entity_uuid uuid,
  import_source_uuid uuid REFERENCES import_source, path text, commit_sha text, git_blob_sha text,
  git_author text, git_utc timestamptz, PRIMARY KEY (entity_type, entity_uuid, import_source_uuid));
CREATE INDEX import_provenance_commit_ix ON import_provenance (import_source_uuid, commit_sha);
CREATE TABLE import_conflict (uuid uuid PRIMARY KEY, import_source_uuid uuid REFERENCES import_source,
  path text, entity_uuid uuid, conflict_kind text NOT NULL, db_head_revision_uuid uuid REFERENCES revision,
  file_sha256 bytea, detail jsonb, resolution text NOT NULL DEFAULT 'pending',
  resolved_by_user_uuid uuid REFERENCES app_user, resolved_utc timestamptz);

-- ---------- immutability (P1)
CREATE FUNCTION forbid_mutation() RETURNS trigger LANGUAGE plpgsql AS
$$ BEGIN RAISE EXCEPTION 'pychron: % is append-only', TG_TABLE_NAME; END $$;
CREATE TRIGGER changeset_append_only BEFORE UPDATE OR DELETE ON changeset FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER revision_append_only BEFORE UPDATE OR DELETE ON revision FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER head_move_append_only BEFORE UPDATE OR DELETE ON head_move FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER bookmark_entry_append_only BEFORE UPDATE OR DELETE ON bookmark_entry FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER intercept_value_append_only BEFORE UPDATE OR DELETE ON intercept_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER baseline_value_append_only BEFORE UPDATE OR DELETE ON baseline_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER blank_value_append_only BEFORE UPDATE OR DELETE ON blank_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER blank_reference_append_only BEFORE UPDATE OR DELETE ON blank_reference FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER icfactor_value_append_only BEFORE UPDATE OR DELETE ON icfactor_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER icfactor_reference_append_only BEFORE UPDATE OR DELETE ON icfactor_reference FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER signal_ref_append_only BEFORE UPDATE OR DELETE ON signal_ref FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER tag_value_append_only BEFORE UPDATE OR DELETE ON tag_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER annotation_value_append_only BEFORE UPDATE OR DELETE ON annotation_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER refpin_value_append_only BEFORE UPDATE OR DELETE ON refpin_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER identity_value_append_only BEFORE UPDATE OR DELETE ON identity_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER cosmogenic_value_append_only BEFORE UPDATE OR DELETE ON cosmogenic_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER ia_value_append_only BEFORE UPDATE OR DELETE ON ia_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER ia_member_append_only BEFORE UPDATE OR DELETE ON ia_member FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER flux_value_append_only BEFORE UPDATE OR DELETE ON flux_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER flux_value_analysis_append_only BEFORE UPDATE OR DELETE ON flux_value_analysis FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER level_z_value_append_only BEFORE UPDATE OR DELETE ON level_z_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER production_meta_append_only BEFORE UPDATE OR DELETE ON production_meta FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER production_value_append_only BEFORE UPDATE OR DELETE ON production_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER level_production_value_append_only BEFORE UPDATE OR DELETE ON level_production_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER chronology_dose_append_only BEFORE UPDATE OR DELETE ON chronology_dose FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER detector_gain_append_only BEFORE UPDATE OR DELETE ON detector_gain FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER sensitivity_value_append_only BEFORE UPDATE OR DELETE ON sensitivity_value FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER holder_meta_append_only BEFORE UPDATE OR DELETE ON holder_meta FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER holder_hole_append_only BEFORE UPDATE OR DELETE ON holder_hole FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER script_version_append_only BEFORE UPDATE OR DELETE ON script_version FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER ref_document_append_only BEFORE UPDATE OR DELETE ON ref_document FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER signal_blob_append_only BEFORE UPDATE OR DELETE ON signal_blob FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER script_text_append_only BEFORE UPDATE OR DELETE ON script_text FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER spectrometer_snapshot_append_only BEFORE UPDATE OR DELETE ON spectrometer_snapshot FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER change_log_append_only BEFORE UPDATE OR DELETE ON change_log FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER change_entity_append_only BEFORE UPDATE OR DELETE ON change_entity FOR EACH ROW EXECUTE FUNCTION forbid_mutation();
CREATE TRIGGER ingest_receipt_append_only BEFORE UPDATE OR DELETE ON ingest_receipt FOR EACH ROW EXECUTE FUNCTION forbid_mutation();

-- analysis: never deleted; updated only in its identity columns, provisional, runid_text and
-- signals_state (sections 5.6, 7.3).
-- @sqlite guard analysis allow identifier_uuid aliquot increment provisional runid_text signals_state
CREATE FUNCTION analysis_guard() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF TG_OP = 'DELETE' THEN
    RAISE EXCEPTION 'pychron: analysis is append-only';
  END IF;
  IF (to_jsonb(NEW) - ARRAY['identifier_uuid','aliquot','increment','provisional','runid_text','signals_state'])
     IS DISTINCT FROM
     (to_jsonb(OLD) - ARRAY['identifier_uuid','aliquot','increment','provisional','runid_text','signals_state']) THEN
    RAISE EXCEPTION 'pychron: analysis is append-only';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER analysis_guard BEFORE UPDATE OR DELETE ON analysis FOR EACH ROW EXECUTE FUNCTION analysis_guard();
