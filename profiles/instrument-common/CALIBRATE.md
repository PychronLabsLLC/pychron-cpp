# Before measuring on the {{ instrument }}

pychron setup wrote a working starting point. These items are placeholders
until you replace them with your instrument's values. `elctl doctor` lists
the files that still say SIMULATION PLACEHOLDER or CONFIRM.

{% if simulation %}
This install runs in **simulation**. When the instrument is ready:
`elctl init --reconfigure --install {{ install_name }} --set simulation=no`.

{% endif %}
1. **Field table** (`tables/`): the magnet settings for each isotope on each
   detector. Replace with a calibration before any magnet move; detector
   protection is planned from it.
2. **Detectors** (`spectrometer.toml` `[[detectors]]`): names, kinds and
   isotopes are defaults for a {{ instrument }}. CONFIRM them against the
   instrument.
3. **Source** (`[source] nominal_hv`), deflections, saturation and
   protection thresholds.
4. **Extraction line** (`extraction_line.toml`, `canvas.toml`): the starter
   line is the five-valve example on simulated transports. Describe your
   line's valves, gauges and their controllers.
5. **Scripts** (`scripts/`): `sim_extract` and `sim_pump` are examples for
   the starter line; write your extraction and pumping scripts.
