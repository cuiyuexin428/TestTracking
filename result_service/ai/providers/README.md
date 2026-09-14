# Provider Adapters

The result service invokes configured local provider commands. The built-in
adapters are:

- `codex`: runs `codex exec --json` with a read-only sandbox and stdin prompt.
- `dsh`: runs the DeepSeek `headless` profile with the prompt as its task.

Provider credentials and user-level profiles stay outside the release package.
The service only sends sanitized result context and never grants providers the
database connection string or arbitrary SQL text.

