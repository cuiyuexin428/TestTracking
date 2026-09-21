# Result Service

The result service is a database read-only HTTP sidecar for the C++ scheduler. It reads
the durable plugin manifest, file outcomes and plugin-owned derived tables. It
does not write client results. Configure a read-only PostgreSQL role through
`RESULT_SERVICE_DB` or `config.example.json`.

Run it from this directory with:

```powershell
python .\launcher.py --config .\config.example.json
```

The assistant embeds only the native DSH Web interface from the `dsh` CLI
installed for the server account. Credentials remain outside the package. DSH
uses its local model configuration and permissions.

Each business plugin owns a declarative `result_view.json` beside its source.
It can also own `result_adapter.py`, which supplies the Python query that builds
the plugin's `analysis` object from its database tables. The framework discovers
both files and only owns their loading and validation contracts.
The result service validates these files against `schema/plugin-view.schema.json`
and exposes them through `/api/plugins` and `/api/plugins/{name}/view`. The UI or
an AI assistant can update a view with `PUT /api/plugins/{name}/view`; the server
accepts only the supported data-only widgets and persists the JSON atomically.
The GET response includes a `revision` content hash. Every PUT must include the
revision read before editing or generating the draft; missing revisions return
HTTP 400. For example, this updates one field while preserving that revision:

```powershell
$viewUrl = 'http://127.0.0.1:56330/api/plugins/coverage/view'
$draft = Invoke-RestMethod -Uri $viewUrl
$draft.title = 'Coverage overview'
Invoke-RestMethod -Uri $viewUrl -Method Put -ContentType 'application/json' `
    -Body ([Text.Encoding]::UTF8.GetBytes(($draft | ConvertTo-Json -Depth 32)))
```

If another operator saves first, the PUT returns HTTP 409 with `error` and
`current_revision`; the stored view is unchanged. Retain the draft, read the
latest definition, and merge the changes before saving against its revision.
Do not replace the draft's revision just to retry. The console retains the
original revision for both manual edits and AI candidates across refreshes and
failed saves. Revision metadata belongs to the HTTP API: it is not persisted in
the plugin JSON or passed to the plugin's `save_view_definition(value)` method.
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

The provider command can be an absolute path in `config.example.json`. For service
launches whose `PATH` does not contain DSH, set `RESULT_SERVICE_DSH_COMMAND`.
The assistant opens the native DSH interface directly. There is no provider selector,
custom chat form, or `/api/ai/chat` endpoint. Neither a selected run nor an available
database, running scheduler, or connected client is required to open DSH.

The panel starts `dsh --profile web` on a loopback port and embeds that UI through
the same-origin `/ai/dsh/<operator_id>/` proxy. HTTP resources, streaming responses
and WebSocket connections all use the main service port; remote browsers never
need to connect to the server's loopback address. DSH owns its conversation UI and
session handling. The service keeps an isolated DSH Web process per active browser
operator, reclaims idle processes, limits concurrency, and stops them with the service.
Native session logs, workspace records and attachments persist separately under
`runtime/result_service/dsh_web/<operator_id>/` in the configured DSH working directory.
Browser profiles receive distinct persistent operator IDs; tabs in one profile
share that profile's identity and conversation history.
Operators share the server account's model settings and filesystem permissions.

DSH's directory picker runs in the browser. The native assistant receives the
service API address, instructions to read each plugin's versioned `AI_CONTEXT.md`,
and the report-generation contract. Reports generated there appear in the result
page on its next refresh (every five seconds).
For report edits, first GET the current `source` and `revision` from
`/api/plugins/<plugin>/report-script`, then PUT the complete `source`, the revision
used for the edit, and the current session `operator_id`. Native DSH receives that
identity as `TESTTRACKING_OPERATOR_ID`. HTTP 409 means another operator changed
the script: keep the draft, read the latest source and revision, and merge before
saving. Do not retry the same draft with the new revision. Generate the changed
report only after its script was saved successfully.

The default listen address is `0.0.0.0:56330`, for trusted LAN use without login.
Open `http://<server-LAN-IP>:56330/` from another machine. The source configuration
is `config.example.json`; packages use `config/result_service.json`.
On Windows, run `tools/open_result_service_firewall.ps1` once as administrator
to allow the configured TCP port from the local subnet. DSH executes under the
server account; visitors do not need the CLI installed locally.
Browser writes must come from the console's own HTTP origin. Foreign or opaque
origins are rejected before commands run; CLI tools without browser origin headers
remain supported. JSON API writes require `Content-Type: application/json`.
Starting DSH requires a JSON POST with the browser's `operator_id`; native HTTP bodies and WebSocket traffic
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

Run commands accept `worker_ids`, an array of exact scheduler client identities
(the `worker_id` values returned by `/api/scheduler`). An empty or omitted array
allows every compatible client of the selected plugin. A nonempty array restricts
dispatch to those exact identities; ID prefixes do not match. Each client currently
enables exactly one plugin and uses its base `worker_id`. The selection is persisted with queued and active runs and restored
after a scheduler restart. It does not change the business work fingerprint, so
choosing different machines cannot bypass duplicate-task prevention.

Duplicate prevention blocks equivalent queued, planning, running or stopped work,
and finished runs with at least one framework task record and all tasks reported
with exit code zero. Missing task evidence does not prove complete success. `finished`
means outcomes were received, including business failures; failed outcomes alone
allow an ordinary retry. An earlier complete success still blocks a duplicate if
the latest attempt failed. Explicit forced reruns require a reason and link to
the latest equivalent run, including one with failed business outcomes. This uses
the framework task records rather than plugin-specific result tables.

Before creating a run, empty strings, whitespace-only strings and empty JSON
objects normalize to the same empty parameters. Nonempty JSON objects use a
stable key order; other custom text is preserved. The scheduler then calls the
plugin's `TaskPlugin::validateRunParameters` before computing the fingerprint or
queueing. The default accepts custom-plugin parameters; plugins own their rules.
Coverage and STP compare currently reject nonempty run parameters. Configure their
program argument templates and task batch sizes in plugin configuration instead.

Each new run also saves an execution fingerprint covering the loaded plugin,
its configuration, manifest, dataset version and parameters. Before a queued run
is planned, the scheduler checks that fingerprint. A changed deployment leaves
the run queued with `blocked: true` and a `blocked_reason`; operators can restore
the original deployment or cancel the queued run. Later runs of that plugin keep
FIFO order. Recovering an unfinished active run under a changed deployment stops
startup with an explicit recovery-blocked error and retains its progress. A
running process continues using its already loaded plugin and frozen task plan;
editing the configuration file takes effect after restart. Older run records
without the execution fingerprint retain their existing work fingerprint and
plugin recovery checks.

Checkpoint, deployment identity and completion-journal read failures also block
startup and preserve the original run; restore storage access and restart with
the original deployment. They are not treated as evidence that the run is missing
or irrecoverable. The new `TaskPlugin::validateRunParameters` C++ interface requires
rebuilding and deploying the scheduler and all plugin DLLs together. Complete
unfinished runs on the old version before upgrading; after a crash, restore using
the original DLLs, manifest and configuration before replacing them.

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

Deployment inputs use the immutable artifact repository configured by
`artifact_store_dir`. Plugin packages, executable program packages and dataset
archives are stored once by SHA-256. Reusing the same kind/name/version with
different bytes returns HTTP 409; an identical upload is idempotent. The API is:

- `POST /api/artifacts/upload?kind=...&name=...&version=...&filename=...` with an
  `application/octet-stream` body and optional `X-Content-SHA256` header.
- `POST /api/artifacts/register` with JSON fields `source_path`, `kind`, `name`,
  `version`, optional `filename`, `metadata`, and `sha256`. Local registration is
  restricted to `artifact_import_roots`. Packages use the dedicated
  `data/artifact_import` staging directory rather than exposing the package tree.
- `GET /api/artifacts` with optional exact `kind`, `name`, `version`, and `sha256`
  filters; `GET /api/artifacts/<artifact_id>` returns one metadata record.
- `GET /api/artifacts/objects/<sha256>` streams immutable content for deployment.

Server packages keep this repository in `data/artifacts`. Artifact metadata is
persisted in `catalog.json`; object bytes are under `objects/<prefix>/<sha256>`.
The repository does not write to the result-analysis database.

Per-client deployment intent and agent reports are persisted beside the artifact
catalog in `deployments.json`. A deployment component has a stable `slot` and an
`artifact_id`; responses expand the artifact metadata so agents can download and
verify the exact SHA-256 object. The API is:

- `GET /api/deployments` lists configured clients; `GET /api/deployments/<client>`
  returns one client, including `unconfigured`, `pending`, `waiting_window`,
  `waiting_idle`, `applying`, `staged`, `failed`, or `current` reconciliation state.
  `history` contains up to 10 previous desired generations, newest first, with
  their revision, components, configuration patch and update time.
- `PUT /api/deployments/<client>/desired` accepts `revision`, `components`,
  `config_patch` (defaults to `{}`), and optional `operator_id` and `reason`.
  Use `null` revision for the first assignment and the
  latest desired revision for later changes. A stale write returns HTTP 409 with
  `current_revision`.
- `POST /api/deployments/preview` accepts `components` and `config_patch`. It
  validates the plan without publishing, returning the normalized patch,
  `program_bindings`, suggested `scheduler_plugins`, `scheduler_revision`, and
  warnings. Artifact-backed coverage and stpcompare executables receive derived
  program versions both here and when the desired generation is saved.
- `POST /api/deployments/bind-scheduler` accepts the same plan plus the reviewed
  `scheduler_revision` and `operator_id`. It saves matching expected program
  versions with revision checking. Changed versions require the affected plugin
  to have no active, queued or recoverable unfinished run; inability to verify
  that state blocks the change. Restart the scheduler after saving. This does
  not publish client intent or restart either process.
- The same desired endpoint accepts `{ "revision": "<current>", "action":
  "rollback", "target_revision": "<historical>", "reason": "<optional>" }`.
  It republishes the saved components and patch as a new generation; components
  and patch cannot be overridden on a rollback request.
- `PUT /api/deployments/<client>/reported` accepts `desired_revision`, overall
  `state`, component states, and optional `agent_version` and `message`. Reports
  must list the complete component plan of the current desired generation.
  Overall states are `waiting_window`, `waiting_idle`, `applying`, `staged`,
  `ready`, and `failed`; component states are `pending`, `downloading`, `staged`,
  `active`, and `failed`. Every component must be active for a ready report.

The agent recursively overlays `config_patch` onto its original client
configuration: objects merge recursively and scalar, array or null values replace
the previous value. Each generation is applied to that original configuration,
so removing a field from a patch restores the original setting. The local
`client.worker_id` and `client.deployment_endpoint` cannot be patched; replacing
the entire `client` object with a non-object is also rejected. Other fields remain
plugin-defined. A path value can use `{artifact:slot}` for the installed artifact
directory or `{artifact:slot}/relative/path` for one of its files, for example:

```json
{
  "plugins": {
    "custom-plugin": {
      "dll": "{artifact:worker-plugin}/worker.dll",
      "tool": "{artifact:engine}/bin/engine.exe",
      "data_root": "{artifact:inputs}"
    }
  }
}
```

References may appear in nested objects or arrays, must occupy the entire string
value, and must name a component slot in the same desired plan. Relative paths
cannot leave the artifact directory. The client updater assigns no plugin-specific
meaning to slot names or configuration fields.

The console offers a deployment form for coverage and stpcompare, including
artifact selection, package-relative paths, the two STP program argument arrays,
per-plugin task concurrency, and separate execution/update windows. The enabled
plugin checklist supports concurrent workers and retains each plugin's form
draft while switching the editor. Unrelated configuration is preserved. Existing desired
plans open in the advanced JSON editor; new plans open in the form. Execution
mode is inherited unless the operator chooses simulation or real execution;
program artifacts require an explicit choice. Coverage also exposes `store_cov`,
preserving the saved value and defaulting to false for a new plan. Preview before
publishing a form. JSON remains editable separately, and conflicts keep the draft
and original revision for review rather than retrying over another operator's
changes.

For artifact-backed coverage and stpcompare programs, `program_bindings.py`
derives `sha256:...` versions from the program archive SHA-256, relative executable
entry point and execution settings. STP includes each role's arguments and
`dry_run`; coverage includes `program_dir`, `opencppcoverage`, `engine_dlls`,
`store_cov` and `dry_run`. Program-artifact dependencies contribute their content
hash and relative path. Human version labels and downloaded cache locations do
not change the identity. The deployment patch must explicitly contain these
settings so clients cannot silently inherit different execution semantics.

The normalized version is written to coverage `program_version` or STP
`programs.<role>.version`. Saving its matching scheduler expected-version fields
and restarting uses the existing registration and duplicate-run checks. Local
paths and custom plugins retain their plugin-owned version conventions;
coverage without `program_version` still reads `program_dir/version.txt`.
Unmanaged dependencies contribute their configured path, not the bytes on a
remote machine. Dataset versions and manifests remain separately configured;
uploading a dataset does not update the scheduler manifest.

Server plugin installation is separate from client deployment:

- `POST /api/server-plugins/preview` accepts `artifact_id` for a plugin ZIP with
  `server_plugin.json`. Upload it as `<plugin>-server`, with a version matching
  the manifest. The response includes files, resources, install/upgrade action,
  warnings and both `scheduler_revision` and `result_service_revision`.
- `POST /api/server-plugins/install` accepts the same artifact and both reviewed
  revisions plus `operator_id`. It rechecks that the affected plugin has no
  unfinished work, creates an independent installation under
  `artifact_store_dir/server-plugins/installations`, snapshots the existing
  analysis resources with the new plugin resources, and switches both config
  references. Backups and a transaction journal recover failed/interrupted
  configuration commits. Existing DLLs and saved user report scripts remain.

New server plugins stay disabled until configured; upgrades preserve existing
settings and an existing dataset manifest. Installation does not load the DLL,
run packaged Python, or restart services. Restart the scheduler and result
service to load the installation. Package format is documented in
[服务端插件包格式](../docs/服务端插件包格式.md). Ordinary client DLL/ZIP uploads do
not qualify for server installation. These operations are per-client or
per-server configuration changes, with no multi-client release transaction or
framework executable self-update.

Desired generations and reports use atomic file replacement and survive service
restart. Repeating the same desired component set and configuration patch, or the
same report, is idempotent. A configuration-only change creates a new revision.
Changing away from and later back to the same component set creates a new generation so an old
agent report cannot be mistaken for the current installation.

Execution plugin configuration and analysis adapters are separate. Adapter
configuration does not prove that the corresponding execution plugin is loaded,
that business executables exist, or that a client is online. Python report and
JSON summary editors continue to work on the result analysis page.
