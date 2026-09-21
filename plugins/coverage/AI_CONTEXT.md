# Coverage plugin

## Purpose

The coverage plugin executes one STP input at a time under OpenCppCoverage and
stores the durable set of covered source-line points. Task execution and result
analysis are separate. Analysis must use the database and must not invoke the
client executable or inspect client work directories.

## Execution configuration

The plugin currently accepts only empty scheduler run `params`. Empty strings,
whitespace and empty JSON objects all mean no run parameters; nonempty values are
rejected before creating a run. Configure program paths, command arguments and
`task_size` in the plugin configuration instead. This does not restrict the
separate `parameters` passed to Python analysis reports.

## Durable data

- `plugin_runs` identifies the run, deployment versions, manifest fingerprint,
  expected file count, and completion status.
- `plugin_run_files` is the durable input manifest for the run. Each row records
  an STP relative path and, for current runs, its byte size and content digest.
- `plugin_file_outcomes` records whether each STP execution was durably accepted.
- `coverage_file_points` stores the covered point set for each STP as packed
  little-endian unsigned 64-bit values in `points`.
- `coverage_cov_files` is optional and stores raw OpenCppCoverage binary output.

An STP is available for analysis only when its outcome is `success` and its
`coverage_file_points` row is present. A complete run has every manifest file in
that state. Never infer completeness from the scheduler status alone.

## Supplied analysis

The packaged Python analysis selects a deterministic compact STP set whose union
matches the union of all successfully stored coverage points. File size metadata
may be used as a stable tie-break or optimization input. Report the algorithm's
guarantee accurately; a greedy set cover is not a guaranteed global minimum.

## Query guidance

Always parameterize `run_id`. Read the live schema before querying because older
runs may predate file size or digest columns. Preserve STP relative paths exactly.
Do not modify raw outcome or point tables from a display report.

When asked to apply an edited report, first GET
`/api/plugins/coverage/report-script` and read its `source` and `revision`. PUT the
complete source, that revision and the current session `operator_id` to the same
endpoint. Native DSH receives the identity as `TESTTRACKING_OPERATOR_ID`. On HTTP
409, preserve the draft, read the latest script and merge changes before saving;
do not retry the unchanged draft with a newer revision. Run the modified report
only after saving it successfully.
