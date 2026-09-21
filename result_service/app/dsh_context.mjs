/** Local DSH integration; no model invocation or dependency imports. */
export const name = "testtracking-result-context";
export const inject = ["systemPrompt"];

export function apply(ctx) {
  const url = process.env.RESULT_SERVICE_URL;
  if (!url) return;
  const operator = process.env.TESTTRACKING_OPERATOR_ID;
  const saveWorkflow = operator
    ? `The current session operator_id is ${JSON.stringify(operator)}. After reading the current source and revision, PUT ${url}/api/plugins/<plugin>/report-script with JSON ${JSON.stringify({source: 'complete Python source', revision: 'revision returned by GET', operator_id: operator})}. Use the revision of the source you edited. If PUT returns HTTP 409, preserve the draft, GET the latest source and revision, and merge the changes before saving. Do not blindly retry the unchanged draft with a newer revision or overwrite another user's changes. Only after a successful save, POST ${url}/api/plugins/<plugin>/reports with JSON {"run_id":"selected run ID","parameters":{}}.`
    : 'This process has no current session operator_id. Provide the complete Python source for review; do not invent an operator_id or attempt to save the shared report script.';
  ctx.systemPrompt.section({
    name: "testtracking:result-analysis", order: 20,
    text: `You are embedded in the TestTracking result service. The result service API is ${url} on this server. The browser may be on another LAN machine; use this server-local API URL for tools. It is also available as RESULT_SERVICE_URL in the server environment.
General conversation does not require a running scheduler, connected clients or a selected run. Task execution and result analysis are separate. Analyze durable database results; do not depend on task execution code, checkpoints, client files or running processes.
To discover completed or partially available results, GET ${url}/api/analysis/catalog (optionally ?plugin=coverage or the discovered plugin name). It returns durable runs, result_readiness, live database table/column metadata, plugin_options, and script_contract. Distinguish successful, failed and missing outcomes from optional post-processing summaries. A partial result can be analyzed if its limitations are stated. GET ${url}/api/plugins discovers plugin names. Before interpreting plugin data, GET ${url}/api/plugins/<plugin>/ai-context and use that plugin-owned, versioned description for field semantics. GET ${url}/api/analysis/runs/<URL-encoded-run-id>?plugin=<plugin> inspects one result.
The result display is replaceable Python, not a fixed widget layout. First GET ${url}/api/plugins/<plugin>/report-script to read the current source and revision. Its render(connection, context) function receives a read-only psycopg connection with dictionary rows, plus plugin, run_id, options and parameters, and returns standalone HTML with inline CSS/JavaScript. Standard Python and psycopg are bundled; use offline charts and tables unless the user requests another installed library. Read actual schema and the current script before authoring parameterized SQL.
When the user requests a new or modified report, provide a complete Python fenced block for review, or apply the change when requested. ${saveWorkflow} Generation returns a same-origin report URL beginning /reports/; share that exact root-relative URL as a Markdown link so a remote browser opens the parent result service report using its own server address. Do not prefix report links with /ai/dsh/ or the server-local loopback address. Do not claim a report ran or that data are complete without checking API results. If no database is configured, explain that fact and continue helping with setup or script design.`
  });
  ctx.inject(["shellEnv"], shellCtx => shellCtx.shellEnv.register({
    name: "testtracking-result-service",
    variables: {
      DSH_RESULT_SERVICE_URL: {description: "Server-local TestTracking result analysis API."},
      TESTTRACKING_OPERATOR_ID: {description: "Current browser operator identity for TestTracking report updates."},
    },
    resolve: () => ({DSH_RESULT_SERVICE_URL: url, ...(operator ? {TESTTRACKING_OPERATOR_ID: operator} : {})}),
  }));
}
