# Observability

How to see an instrument on the lab's monitoring box: its pressures and
valves, what the queue is doing, and whether pychron itself is healthy.
Written for the lab manager. You need a text editor, a terminal on the box
and about half an hour.

## What you get

Each lab has one monitoring box from PychronLabs, on the lab network, running
Prometheus (which collects numbers) and Grafana (which draws them). Pychron
publishes its numbers on the instrument computer; the box fetches them every
fifteen seconds.

| Dashboard | For | Shows |
|---|---|---|
| Instrument health | lab staff | pressures, cryostat temperatures, heaters, valve states, how fresh each reading is, alarms |
| Run operations | the lab manager | whether a queue is running and how far it is, how runs end, how long each phase takes, conditionals that tripped, notifications sent |
| Application health | whoever supports the lab | the version running, transports, warnings and errors in the log, the scheduler, how long saving takes |

And one alert: pychron stopped while a queue was running. Pychron's own alarms
and [notifications](notifications.md) tell you about everything else; this is
the one thing it cannot tell you itself.

The numbers exist only while pychron is running. Close it and the dashboards
show a gap.

## 1. Turn metrics on

On the instrument computer, open the line's `extraction_line.toml` and add:

```toml
[metrics]
enabled = true
bind = "0.0.0.0"
port = 9464
```

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | nothing is published, and no port is opened, until this is `true` |
| `bind` | `"0.0.0.0"` | which of the computer's network addresses to listen on. `"0.0.0.0"` is all of them; give one address to listen there only; `"127.0.0.1"` is this computer only. An address, not a name |
| `port` | `9464` | 1 to 65535 |

Or, without a text editor: **File > Preferences… > Metrics**, tick **Publish
metrics for the lab's monitoring box**, choose **Every address of this
computer**, press OK. That writes the same three settings into the line's
local file (`extraction_line.local.toml`), for this computer only.

Restart pychron. The log says:

```
metrics: listening on 0.0.0.0:9464
```

Check it from the box (use the instrument computer's address):

```bash
curl http://INSTRUMENT_PC:9464/metrics
```

You should see several dozen lines that begin `pychron_`, among them one
`pychron_pressure` line for each gauge. If instead:

- **the log says `metrics endpoint is off`** (there is an alarm too): the
  port is taken, usually by a second pychron on the same computer, or `bind`
  is not one of this computer's addresses. Pychron runs normally without the
  endpoint. Close the other copy, or choose another `port`, and restart.
- **`curl` cannot connect**: a firewall on the instrument computer is closing
  the port. Allow incoming TCP on 9464 from the box's address.
- **`curl` works on the instrument computer but not from the box**: `bind` is
  `"127.0.0.1"`, or the two are on different networks.

## 2. Tell the box where to look

On the box, add the job in
[`packaging/observability/prometheus/pychron.scrape.yml`](../packaging/observability/prometheus/pychron.scrape.yml)
under `scrape_configs:` in `prometheus.yml` (usually
`/etc/prometheus/prometheus.yml`), with your values:

```yaml
scrape_configs:
  - job_name: pychron
    scrape_interval: 15s
    static_configs:
      - targets: ["192.168.1.20:9464"]
        labels:
          instrument: "argus"
```

- `targets` is the instrument computer's address and the port from step 1.
- `instrument` is the name the dashboards show. Choose it once: changing it
  later starts a new history.
- Leave `job_name` as `pychron`. The alert looks for that name.
- A second instrument is a second entry under `static_configs`, with its own
  `targets` and `instrument`.

Reload Prometheus (`sudo systemctl reload prometheus`), then open
`http://BOX:9090/targets`. The `pychron` job should say **UP** within half a
minute. **DOWN** with "connection refused" or a timeout is the firewall or
`bind` from step 1.

## 3. Load the dashboards

On the box, from a copy of `packaging/observability/grafana/`:

```bash
sudo mkdir -p /var/lib/grafana/dashboards/pychron
sudo cp dashboards/*.json /var/lib/grafana/dashboards/pychron/
sudo cp provisioning/dashboards/pychron.yml /etc/grafana/provisioning/dashboards/
sudo systemctl restart grafana-server
```

In Grafana, open **Dashboards**, then the **Pychron** folder. Each dashboard
has two menus at the top: **Data source** (the box's Prometheus) and
**Instrument** (the name from step 2).

These paths are those of the Debian and Ubuntu packages. If the box runs
Grafana in a container, the two directories are wherever its configuration
mounts `/etc/grafana/provisioning` and the dashboards path named in
`pychron.yml`.

If a dashboard opens with every panel empty: the **Instrument** menu is
empty too, which means Prometheus has no `pychron` data yet (go back to
step 2), or the scrape job has no `instrument` label.

## 4. The alert

Two rules, in
[`provisioning/alerting/pychron-deadman.yml`](../packaging/observability/grafana/provisioning/alerting/pychron-deadman.yml):

| Rule | Fires when |
|---|---|
| `PychronGone` | the box cannot reach pychron, and a queue was running the last time it could: pychron crashed or froze, the computer is off or asleep, the network is down |
| `PychronStuck` | pychron answers, a queue is running, and its scheduler has done nothing for a minute |

Each waits two minutes before firing, so a restart does not page anyone.
Quitting pychron with no queue running fires nothing.

**What they do not catch:** a queue that hangs inside one run while the rest
of pychron keeps working. The *Since the last run finished* panel on the Run
operations dashboard is where to look for that.

**One false alarm to know:** if pychron is quit within a few seconds of a
queue ending, the box's last reading still says a queue was running, and
`PychronGone` fires until pychron is started again (or for a day).

The clocks of the box and the instrument computer do not have to agree. Every
"how long ago" on the dashboards and in the alert is measured on the
instrument computer.

To install:

1. Find the data source's identifier. In Grafana, **Connections > Data
   sources**, click the Prometheus one. The address in the browser ends
   `/datasources/edit/XXXXXXXX`: that last part is the identifier.
2. Give it to Grafana as `PYCHRON_PROM_UID`. With the Debian and Ubuntu
   packages, add a line to `/etc/default/grafana-server`:

   ```
   PYCHRON_PROM_UID=XXXXXXXX
   ```

3. Copy the rules and restart:

   ```bash
   sudo cp provisioning/alerting/pychron-deadman.yml /etc/grafana/provisioning/alerting/
   sudo systemctl restart grafana-server
   ```

4. Say who is told. In Grafana, **Alerting > Contact points**: edit the
   default contact point and add the lab's email addresses or a Slack
   webhook. For email, Grafana needs a mail server in its own configuration
   (`[smtp]` in `grafana.ini`); the mail service you set up for
   [notifications](notifications.md) will do.
5. Test it. Start a simulated queue (`--sim`), wait until the Run operations
   dashboard says the queue is running, then quit pychron. Within about three
   minutes **Alerting > Alert rules** shows `PychronGone` firing and the
   contact point receives it. Read the message: it should name the
   instrument. Start pychron again and it clears.

Menu names are those of Grafana 12, as of October 2026. The dashboards and
both rules were loaded and the rules seen to fire on Grafana 12.1 with
Prometheus 3.5.

## Try it without a box

`packaging/observability/box` is a monitoring box for your own computer: the
same Prometheus and Grafana in two containers, loaded with the very files a
real box gets. Use it to see the dashboards against a simulated line, or to
check a changed dashboard or alert before it goes to a lab. It needs Docker
(Docker Desktop, or Colima on a Mac).

1. Turn metrics on for the line you will run (step 1 above). On a Mac,
   `bind = "127.0.0.1"` is enough.
2. Start the box:

   ```bash
   cd packaging/observability/box
   docker compose up -d
   ```

   (`docker-compose up -d` with the older, separate program.)
3. Start pychron on that line, simulated: `--sim`.
4. Open <http://localhost:3000>: no sign-in, dashboards under **Pychron**,
   **Instrument** set to `sim`. <http://localhost:9090/targets> shows the
   scrape; **UP** within half a minute.
5. When done: `docker compose down`. Add `-v` to throw its history away too.

What it scrapes is in `box/targets.yml`, by default a pychron on this
computer at port 9464. Change the address there to point it at another
computer; Prometheus reads the file again within half a minute.

On Linux without Docker Desktop, two things differ: uncomment the
`extra_hosts` lines in `docker-compose.yml`, and set `bind = "0.0.0.0"`,
since the container reaches your computer over a network address, not
`127.0.0.1`.

Grafana here lets anyone who can reach it change anything, so both programs
listen on this computer only. It is for trying things, not for a lab.

To try the alert: start a queue, wait for the Run operations dashboard to
say it is running, quit pychron. `PychronGone` fires within about three
minutes (**Alerting > Alert rules**). No contact point is set up here, so
nothing is sent; the rule's state is what to look at.

## Who can read this

The endpoint is read-only. It publishes operational numbers and the names of
the devices in your configuration: pressures, valve states, counts of runs,
the names of transports. It publishes no sample identifier, no project, no
user name and no password or key.

Anyone on the lab network who can reach the port can read it; there is no
password. If that is too open for your lab, set `bind` to the address of the
network interface that faces the box, or have the instrument computer's
firewall accept the port only from the box's address. It is off until you
turn it on.

## What is published

Every name begins `pychron_`. Prometheus adds `instrument` (from step 2) to
each.

### The instrument

| Metric | Labels | Meaning |
|---|---|---|
| `pychron_pressure` | `gauge`, `unit` | a gauge's last reading, in the unit it is configured with |
| `pychron_temperature_kelvin` | `source`, `input` | a temperature controller's input |
| `pychron_heater_readback`, `pychron_heater_setpoint` | `heater` | in the heater's configured unit; absent when the heater does not report it |
| `pychron_heater_enabled` | `heater` | 1 while the output is on |
| `pychron_valve_state` | `valve`, `state` (`open`, `closed`, `unknown`) | 1 for the state the valve is in |
| `pychron_valve_transitions_total` | `valve` | times the valve changed state |
| `pychron_actuation_failures_total` | `valve` | actuations that failed; `unknown` for a switch the line does not have |
| `pychron_last_sample_age_seconds` | `kind`, `source` | seconds since the source was last read; a reading is only as fresh as this |
| `pychron_alarms_total` | `source`, `severity` | alarms raised |

### Runs and queues

| Metric | Labels | Meaning |
|---|---|---|
| `pychron_queue_active` | | 1 while a queue is running |
| `pychron_executor_state` | `state` (`idle`, `preparing`, `running`, `stopping`, `cancelling`, `aborting`, `finalizing`) | 1 for the state the executor is in |
| `pychron_queue_runs` | `status` (`total`, `done`) | the running queue's size and progress |
| `pychron_queues_ended_total` | `end` | queues ended, by how |
| `pychron_runs_started_total` | | |
| `pychron_runs_finished_total` | `state`, `truncated` | runs ended, by their last state |
| `pychron_run_save_errors_total` | | runs whose record could not be saved |
| `pychron_run_state_duration_seconds` | `state` | a histogram of the time runs spend in each state; `saving` is the cost of writing the record |
| `pychron_measurement_blocks_total` | `block`, `ok` | measurement blocks finished |
| `pychron_conditional_trips_total` | `kind`, `level` | conditionals that tripped |
| `pychron_executor_waits_total` | `reason` | times the executor waited, by what for |
| `pychron_last_run_finished_age_seconds` | | seconds since a run last finished; absent until one has |
| `pychron_notifications_total` | `channel`, `event`, `ok` | notifications handed to a channel |

Durations are measured on the line's clock: in a simulation run faster than
real time they are simulated seconds.

### The application

| Metric | Labels | Meaning |
|---|---|---|
| `pychron_build_info` | `version`, `os`, `compiler` | always 1; the labels say what is running |
| `pychron_process_uptime_seconds` | | seconds since pychron started |
| `pychron_log_records_total` | `level`, `component` | log records; `component` is the first part of the logger's name |
| `pychron_transport_connected` | `transport` | 1 while the transport is up, 0 while it is down |
| `pychron_transport_outages_total` | `transport` | times the transport went down. Single errors are not counted: the log has them |
| `pychron_scheduler_job_runs_total`, `_failures_total`, `_skipped_overlaps_total` | `job` | each periodic job's counts |
| `pychron_scheduler_heartbeat_age_seconds` | | seconds since the scheduler last ran its heartbeat job |
| `pychron_metrics_scrape_duration_seconds` | | how long the last scrape took to answer |
| `pychron_metrics_bad_requests_total` | | connections to the endpoint that were dropped |
| `pychron_metrics_dropped_series_total` | | series refused (more than 1000 in one metric: a configuration with a great many devices, or a bug) |

Counters start again from zero when pychron restarts. The dashboards show
increases over a time range, which is unaffected.

A counter appears when pychron first hears of what it counts: a valve, a
gauge, a transport, a notification channel. The first alarm from a source
that has never been read (the line itself, say) is therefore not drawn as an
increase; every later one is.
