# STP compare plugin

## Purpose

For each STP input, the plugin executes two independently versioned programs:
`baseline` and `candidate`. Each program receives `--input <stp>` and
`--output-dir <directory>` and writes `info.txt` and `info_new.txt` below its
private output directory.

The execution plugin only collects and persists raw outputs. It does not compare
the files or decide whether either implementation is better. Comparison belongs
to replaceable Python analysis created for the deployment.

## Execution configuration

The plugin currently accepts only empty scheduler run `params`. Empty strings,
whitespace and empty JSON objects all mean no run parameters; nonempty values are
rejected before creating a run. Configure baseline/candidate executable paths,
argument templates and `task_size` in the plugin configuration instead. Python
analysis reports may independently define their own `parameters`.

## Durable data

Every successful STP has four raw output rows, identified by two dimensions:

- `program_role`: `baseline` or `candidate`.
- `export_stage`: `first` or `second`.

Raw file bytes, original filename, byte count, content digest, program version,
and attempt identity are stored by the plugin. `plugin_file_outcomes` is marked
`success` only when all four outputs are committed. Missing any one output makes
the STP incomplete.

Use the live database catalog to discover the configured result table and exact
column names. Treat raw content as bytes and decode it explicitly in analysis;
do not assume the database or execution plugin normalized text encoding or line
endings.

## Analysis guidance

Analysis may compare either export stage within one program or corresponding
stages across programs. State the chosen comparison in the generated report.
Never depend on client work directories or rerun either executable during
analysis.

When asked to apply an edited report, first GET
`/api/plugins/stpcompare/report-script` and read its `source` and `revision`. PUT
the complete source, that revision and the current session `operator_id` to the
same endpoint. Native DSH receives the identity as `TESTTRACKING_OPERATOR_ID`.
On HTTP 409, preserve the draft, read the latest script and merge changes before
saving; do not retry the unchanged draft with a newer revision. Run the modified
report only after saving it successfully.
