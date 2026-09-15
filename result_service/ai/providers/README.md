# Provider Adapters

The result service invokes configured local provider commands. The built-in
adapters are:

- `codex`: runs `codex exec --json` with a stdin prompt. Approval and sandbox
  policy come from the local Codex configuration.
- `dsh`: runs the DeepSeek `headless` profile with the prompt as its task.

Provider credentials and user-level profiles stay outside the release package.
The service sends sanitized result context and never forwards credentials or
connection strings. The provider otherwise retains the permissions granted by
its local CLI configuration.
