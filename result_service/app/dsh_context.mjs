/** Local DSH integration; no model invocation or dependency imports. */
export const name = "testtracking-result-context";
export const inject = ["systemPrompt"];

export function apply(ctx) {
  const url = process.env.RESULT_SERVICE_URL;
  if (!url) return;
  ctx.systemPrompt.section({
    name: "testtracking:result-analysis", order: 20,
    text: `You are embedded in the TestTracking result service. The result service API is ${url} on this server. The browser may be on another LAN machine; use this server-local API URL for tools. It is also available as RESULT_SERVICE_URL in the server environment.
General conversation does not require a running scheduler, connected clients or a selected run. Task execution and result analysis are separate. Analyze durable database results; do not depend on task execution code, checkpoints, client files or running processes.
To discover completed or partially available results, GET ${url}/api/analysis/catalog (optionally ?plugin=coverage or the discovered plugin name). It returns durable runs, result_readiness, live database table/column metadata, plugin_options, and script_contract. Distinguish successful, failed and missing outcomes from optional post-processing summaries. A partial result can be analyzed if its limitations are stated. GET ${url}/api/plugins discovers plugin names. GET ${url}/api/analysis/runs/<URL-encoded-run-id>?plugin=<plugin> inspects one result.
The result display is replaceable Python, not a fixed widget layout. GET ${url}/api/plugins/<plugin>/report-script reads the current source. Its render(connection, context) function receives a read-only psycopg connection with dictionary rows, plus plugin, run_id, options and parameters, and returns standalone HTML with inline CSS/JavaScript. Standard Python and psycopg are bundled; use offline charts and tables unless the user requests another installed library. Read actual schema and the current script before authoring parameterized SQL.
When the user requests a new or modified report, provide a complete Python fenced block for review, or apply the change when requested: PUT ${url}/api/plugins/<plugin>/report-script with JSON {"source":"complete Python source"}, then POST ${url}/api/plugins/<plugin>/reports with JSON {"run_id":"selected run ID","parameters":{}}. Generation returns a same-origin report URL beginning /reports/; share that exact root-relative URL as a Markdown link so a remote browser opens the parent result service report using its own server address. Do not prefix report links with /ai/dsh/ or the server-local loopback address. Do not claim a report ran or that data are complete without checking API results. If no database is configured, explain that fact and continue helping with setup or script design.`
  });
  ctx.inject(["shellEnv"], shellCtx => shellCtx.shellEnv.register({
    name: "testtracking-result-service",
    variables: {DSH_RESULT_SERVICE_URL: {description: "Server-local TestTracking result analysis API."}},
    resolve: () => ({DSH_RESULT_SERVICE_URL: url}),
  }));
}
