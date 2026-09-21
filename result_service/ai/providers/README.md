# Native DSH Integration

The result service supports only the native DSH Web interface. It starts the
configured local `dsh` command with the `web` profile and exposes an isolated
native Web process for each browser operator through the service's same-origin
proxy. There is no custom chat endpoint or headless chat mode in the console.

Each operator has durable session logs, workspace records and attachments under
`runtime/result_service/dsh_web/<operator_id>/` in the DSH working directory.
The launcher overrides those native storage paths; separate processes sharing
the default DSH home alone would expose the same session list. Model settings,
credentials, skills and profile patches continue to use the installed DSH home.
Operator IDs persist in browser localStorage, so tabs of one browser profile
share its identity; different users should use their own browser profiles.

DSH availability depends on the installed command, not on task execution,
connected clients, a selected run, or database readiness. Provider credentials
and user-level profiles stay outside the release package. Native DSH retains the
permissions and model configuration of the server account.

The injected context links to the result-service APIs, plugin-owned
`AI_CONTEXT.md` descriptions, and the versioned Python report editing workflow.
Session isolation does not isolate the shared project files or database.
