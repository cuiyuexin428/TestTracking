# Native DSH Integration

The result service supports only the native DSH Web interface. It starts the
configured local `dsh` command with the `web` profile and exposes an isolated
native Web process for each browser operator through the service's same-origin
proxy. There is no custom chat endpoint or headless chat mode in the console.

DSH availability depends on the installed command, not on task execution,
connected clients, a selected run, or database readiness. Provider credentials
and user-level profiles stay outside the release package. Native DSH retains the
permissions and model configuration of the server account.

The injected context links to the result-service APIs, plugin-owned
`AI_CONTEXT.md` descriptions, and the versioned Python report editing workflow.
Session isolation does not isolate the shared project files or database.
