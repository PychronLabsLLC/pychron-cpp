-- A sample's location is one PostGIS point (WGS 84, longitude then latitude)
-- instead of two columns. The store reads it as ST_AsEWKT and writes EWKT
-- ('SRID=4326;POINT(lon lat)'); the catalog API still speaks lat and lon.
-- PostGIS 3 is a trusted extension: the database owner may create it. In
-- SQLite the same EWKT is kept as text (no PostGIS there).
-- @sqlite skip
CREATE EXTENSION IF NOT EXISTS postgis WITH SCHEMA public;
ALTER TABLE sample ADD COLUMN geom public.geometry(Point, 4326);
-- @sqlite skip
UPDATE sample SET geom = public.ST_SetSRID(public.ST_MakePoint(lon, lat), 4326)
  WHERE lat IS NOT NULL AND lon IS NOT NULL;
-- @sqlite exec UPDATE sample SET geom = 'SRID=4326;POINT(' || lon || ' ' || lat || ')' WHERE lat IS NOT NULL AND lon IS NOT NULL
ALTER TABLE sample DROP COLUMN lat;
ALTER TABLE sample DROP COLUMN lon;
-- @sqlite skip
CREATE INDEX sample_geom_gix ON sample USING GIST (geom);
