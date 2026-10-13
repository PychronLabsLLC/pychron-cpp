# Metrics (`libs/metrics`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- `libs/metrics` (Prometheus endpoint, `[metrics]`) is Qt-free and builds
  everywhere; `-DPYCHRON_METRICS=OFF` skips it and the exporters. Metrics come
  from bus events (`CoreExporter`, `ExperimentMetrics` in `libs/experiment`),
  not from instrumenting the control path. Three rules:
  (1) a label value is a configured name or an enumeration, never a run id,
  an identifier or a message, and a counter is created at zero as soon as
  what it counts is known (one that first appears at 1 shows no increase);
  (2) a duration is a difference of event `ts` (the line's clock may be
  simulated), and an age is measured here on `RealClock` and exported as
  seconds, never as a timestamp for the box to subtract from its own clock;
  (3) a metric added to an exporter needs a panel in
  `packaging/observability/grafana/dashboards`, and a renamed one its panel
  renamed (`MetricsPackaging` in `tests/integration` fails otherwise).
  Tests check names, not that a query returns anything or that Grafana
  accepts a file: after changing a dashboard or the alert, load it in the
  virtual box (`packaging/observability/box`, `docker compose up -d`) and
  look. User guide: `docs/observability.md`.
