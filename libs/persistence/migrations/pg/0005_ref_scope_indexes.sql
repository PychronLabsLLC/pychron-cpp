-- An analysis's reference values are found by what they belong to: the flux
-- of its position, the geometry and production of its level, the chronology
-- of its irradiation, the gains and sensitivity of its mass spectrometer.
-- ref_object had an index on (ref_type, key) only, so each lookup read every
-- object of the type: tens of thousands of flux positions for one analysis,
-- and once for every analysis loaded. One index per scope column; partial,
-- since each column is set for its own types only.
CREATE INDEX ref_object_position_ix ON ref_object (position_uuid) WHERE position_uuid IS NOT NULL;
CREATE INDEX ref_object_level_ix ON ref_object (level_uuid) WHERE level_uuid IS NOT NULL;
CREATE INDEX ref_object_irradiation_ix ON ref_object (irradiation_uuid) WHERE irradiation_uuid IS NOT NULL;
CREATE INDEX ref_object_mass_spectrometer_ix ON ref_object (mass_spectrometer_uuid)
  WHERE mass_spectrometer_uuid IS NOT NULL;
