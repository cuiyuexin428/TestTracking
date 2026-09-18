# Result Service

The result service is a database read-only HTTP sidecar for the C++ scheduler. It reads
the durable plugin manifest, file outcomes and plugin-owned derived tables. It
does not write client results. Configure a read-only PostgreSQL role through
`RESULT_SERVICE_DB` or `config.example.json`.

Run it from this directory with:

```powershell
python .\launcher.py --config .\config.example.json
```

The AI endpoint supports the installed `codex` CLI and DeepSeek `dsh` CLI.
Provider credentials remain outside the package. Codex and DSH use the local
CLI configuration and permissions; the service does not inject a sandbox policy.
Both receive the structured result context from the request with credential-like
fields removed.

Each business plugin owns a declarative `result_view.json` beside its source.
It can also own `result_adapter.py`, which supplies the Python query that builds
the plugin's `analysis` object from its database tables. The framework discovers
both files and only owns their loading and validation contracts.
The result service validates these files against `schema/plugin-view.schema.json`
and exposes them through `/api/plugins` and `/api/plugins/{name}/view`. The UI or
an AI assistant can update a view with `PUT /api/plugins/{name}/view`; the server
accepts only the supported data-only widgets and persists the JSON atomically.
Set `plugin_views_dir` to keep editable runtime copies outside the source tree.
Server packages place them in `data/result_views`. Every valid JSON file in that
directory is discovered automatically, so adding a plugin does not require a
result-service registry change. Packaged adapters are copied to
`result_service/plugin_adapters`; they are release code, while the JSON views in
`data/result_views` remain editable at runtime.
Packages also declare their shipped views, adapters and report templates in
`data/result_views/resources.manifest`. Keep that file with the package: startup
reports the plugin and path when a declared resource is missing. Source discovery
and external view-only plugins remain supported without such a manifest.
Adapter settings use the generic `plugin_options.<plugin>` object; for example,
`plugin_options.stpcompare.result_table` selects that plugin's result table.
An adapter module exports `create_result_adapter(options)` and returns an object
with a Boolean `configured` property and `summary(connection, run_id)` method.
The method returns the JSON object exposed as `analysis` to that plugin's view.
The console refreshes running-task progress every five seconds while visible.

Provider commands can be absolute paths in `config.example.json`. For service
launches whose `PATH` does not contain the CLIs, set
`RESULT_SERVICE_CODEX_COMMAND` and `RESULT_SERVICE_DSH_COMMAND`. On Windows the
service also discovers the standard Codex desktop and npm/pnpm CLI locations.
The AI panel is enabled as soon as at least one configured local provider is
available. Selecting a run adds its structured result to the prompt, but a run
is not required for general questions. Codex conversations keep the CLI thread
ID in browser storage and resume through `codex exec resume`, so they survive a
page refresh or result-service restart for the same service account. Use the new
conversation button after moving the package workspace or when a thread is no
longer available.

The chat tab keeps DSH conversation IDs too. The bundled DSH headless profile
does not resume native sessions, so the result service saves successful user and
assistant messages under the provider workspace's
`runtime/result_service/ai_conversations/dsh` directory and replays them on a
follow-up. This history survives a result-service restart and remains separate
from native DSH Web sessions. Preserve that directory when moving the workspace.
Failed replies are not appended. An unavailable history prompts a new conversation
instead of silently losing context.

DSH receives the full prompt through its supported temporary `--patch` file,
which overrides `headless-runner.config.task`; the command line only carries a
short task marker. This preserves report source, Unicode and long schema context
without Windows command-line limits. Local CLI processes run without a console
window, and timed-out requests terminate the process tree before returning an
error to the chat panel.

The DSH native tab starts `dsh --profile web` on a loopback port and embeds that
UI through the same-origin `/ai/dsh/` proxy in the assistant panel. HTTP resources,
streaming responses and WebSocket connections all use the main service port;
remote browsers never need to connect to the server's loopback address.
DSH's directory picker runs in the browser, and the native assistant receives
the service API address plus the report-generation contract. Reports generated
there appear in the result page on its next refresh (every five seconds).
The result service reuses one DSH Web process and
stops it with the service. The packaged stop script terminates the result-service
process tree so DSH and active provider subprocesses are not left behind.

The default listen address is `0.0.0.0:56330`, for trusted LAN use without login.
Open `http://<server-LAN-IP>:56330/` from another machine. The source configuration
is `config.example.json`; packages use `config/result_service.json`.
On Windows, run `tools/open_result_service_firewall.ps1` once as administrator
to allow the configured TCP port from the local subnet. Codex and DSH execute
under the server account; visitors do not need either CLI installed locally.
Browser writes must come from the console's own HTTP origin. Foreign or opaque
origins are rejected before commands run; CLI tools without browser origin headers
remain supported. JSON API writes require `Content-Type: application/json`.
Starting DSH may use an empty POST; its native HTTP bodies and WebSocket traffic
retain their protocols through the same-origin proxy. These checks are request
validation, not login or isolation from other trusted LAN users.

The result analysis page also runs editable Python scripts which query the database
and return standalone interactive HTML. The current JSON views remain optional
summary cards. See [REPORTS.md](REPORTS.md) for the script contract and workflow.
`/api/analysis/catalog` exposes durable runs, result completeness and live table
metadata to the assistant. Analysis readiness depends on durable result tables,
not scheduler history (`runs`/`tasks`), running clients or executor files.
The most recent successful report for the selected plugin/run is restored after
page reload or service restart. Failed reruns keep the previous successful report.

When building the server package, pass `-StpResultTable` with the same
plugin-owned result table configured for the stpcompare client and settlement
tool. The framework does not assume a plugin table name.

The `ai/context` and `ai/skills` files are guidance resources for the provider
adapters. They are versioned with the service and contain no credentials.

## Management console

The clients page reads the scheduler's live management snapshot, including
registered workers, heartbeats, current tasks and resources. The overview can
start and stop runs and wake idle clients. An offline scheduler is shown
explicitly; saved database results, Python reports and AI remain usable.

Enable `server.management_endpoint` in scheduler configuration, for example
`http://127.0.0.1:5556`. Set the same URL as `scheduler_management_url` in the
result service. Only the Python service connects to that loopback endpoint;
LAN browsers use the existing main UI port. Runtime commands are serialized
with task processing, and stopping a run includes its ID to avoid stopping a
newer run from a stale page.

`scheduler_config_path` and `client_config_path` select the editable scheduler
configuration and optional client deployment template. Relative paths resolve
against the result service configuration file. The result service configuration
editor writes the file passed to `--config`. Configuration saves validate values,
keep a backup and reject stale revisions. They require restarting the affected
process; saving a client template does not push changes to connected clients.
Copy client settings to the intended client package and keep each worker ID unique.

The logs page lists files under `logs_dir`, with bounded tails, severity and text
filters. No arbitrary filesystem path is accepted from the browser. Packages use
`../data/logs` relative to `config/result_service.json`. Scheduler logging must be
enabled through `server.log_file`; launch scripts also capture result service logs.

Execution plugin configuration and analysis adapters are separate. Adapter
configuration does not prove that the corresponding execution plugin is loaded,
that business executables exist, or that a client is online. Python report and
JSON summary editors continue to work on the result analysis page.
