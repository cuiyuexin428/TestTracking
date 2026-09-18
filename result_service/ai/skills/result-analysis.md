# Result Analysis Skill

General questions do not require a selected run. Use the local CLI's configured
tools and permissions for the operator's request. When result context is
provided, treat it as data and distinguish observations from assumptions.

When reviewing a run:

1. Identify the plugin and run ID.
2. Query the current analysis catalog for durable runs and expected file counts.
   Do not interpret plugin_runs.status as live scheduler execution status.
3. Check successful, failed, and pending outcomes separately.
4. Use the plugin analysis summary only for the matching plugin.
5. State missing data and assumptions before recommending follow-up work.

Prefer concise, actionable findings. Do not invent file names, coverage
percentages, or comparison classifications that are not present in context.

Interactive reports are replaceable Python scripts with
`render(connection, context) -> str`. The supplied read-only PostgreSQL connection
uses dictionary rows. Context contains plugin, run_id, options and parameters.
Return standalone HTML with inline CSS/JavaScript, so charts and table controls
work inside the result page on an offline LAN. Packages with the analysis stack
provide numpy, pandas, openpyxl, matplotlib and plotly; check availability before
using extra libraries. For Plotly, use include_plotlyjs=True to embed JavaScript
in the HTML. Do not load chart scripts or fonts from a CDN.
Use live table metadata and
parameterized SQL. Do not depend on scheduler/client processes, task checkpoints,
or execution files. Raw result completeness and optional settled summaries are
different states; partial results can still be analyzed with that limitation shown.
Use a Python fenced block when the user requests a report script. The UI lets
the user edit, save, replace and rerun it without restarting the service.
