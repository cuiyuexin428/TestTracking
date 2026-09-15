# Result Service

The result service is a read-only HTTP sidecar for the C++ scheduler. It reads
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

The DSH native tab starts `dsh --profile web` on a loopback port and embeds that
UI in the assistant panel. The result service reuses one DSH Web process and
stops it with the service. The packaged stop script terminates the result-service
process tree so DSH and active provider subprocesses are not left behind.

When building the server package, pass `-StpResultTable` with the same
plugin-owned result table configured for the stpcompare client and settlement
tool. The framework does not assume a plugin table name.

The `ai/context` and `ai/skills` files are guidance resources for the provider
adapters. They are versioned with the service and contain no credentials.
