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
Provider credentials remain outside the package. Codex runs read-only and DSH
uses its `headless` profile; both receive only the structured result context
from the request.

The `ai/context` and `ai/skills` files are guidance resources for the provider
adapters. They are versioned with the service and contain no credentials.
