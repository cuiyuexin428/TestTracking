from __future__ import annotations

import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

from .config import ProviderConfig


class ProviderError(RuntimeError):
    """A provider could not return a response."""


@dataclass(frozen=True)
class ProviderResult:
    provider: str
    text: str
    raw_output: str = ""


_BLOCKED_CONTEXT_KEYS = {
    "password", "passwd", "secret", "token", "api_key", "db_conn",
    "connection_string", "sql", "query", "command", "env",
}


def sanitize_context(value: Any, *, max_bytes: int = 32_000) -> dict[str, Any]:
    """Strip secrets and execution instructions before invoking an AI tool."""
    def scrub(item: Any, depth: int = 0) -> Any:
        if depth > 6:
            return "<depth-limit>"
        if isinstance(item, Mapping):
            return {str(key): scrub(child, depth + 1)
                    for key, child in item.items()
                    if str(key).lower() not in _BLOCKED_CONTEXT_KEYS}
        if isinstance(item, list):
            return [scrub(child, depth + 1) for child in item[:200]]
        if isinstance(item, (str, int, float, bool)) or item is None:
            return item
        return str(item)

    result = scrub(value if isinstance(value, Mapping) else {})
    if len(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode("utf-8")) > max_bytes:
        raise ProviderError("AI context is too large")
    return result


def _command_argv(config: ProviderConfig) -> list[str]:
    executable = shutil.which(config.command) or config.command
    suffix = Path(executable).suffix.lower()
    if os.name == "nt" and suffix == ".ps1":
        return ["pwsh", "-NoProfile", "-File", executable, *config.args]
    if os.name == "nt" and suffix in {".cmd", ".bat"}:
        return ["cmd.exe", "/d", "/c", executable, *config.args]
    return [executable, *config.args]


def _event_text(event: Any) -> str:
    if isinstance(event, str):
        return event
    if not isinstance(event, dict):
        return ""
    for key in ("text", "message", "content", "value"):
        value = event.get(key)
        if isinstance(value, str):
            return value
        if isinstance(value, list):
            chunks = [_event_text(item) for item in value]
            return "".join(chunk for chunk in chunks if chunk)
        if isinstance(value, dict):
            nested = _event_text(value)
            if nested:
                return nested
    item = event.get("item")
    return _event_text(item) if item is not None else ""


def parse_provider_output(provider: str, output: str) -> str:
    """Extract the final assistant text from plain text or JSONL CLI output."""
    texts: list[str] = []
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        try:
            event = json.loads(stripped)
        except json.JSONDecodeError:
            texts.append(stripped)
            continue
        event_type = event.get("type") if isinstance(event, dict) else None
        if event_type in {"item.completed", "response.output_text.done", "message"}:
            text = _event_text(event)
            if text:
                texts.append(text)
        elif event_type is None:
            text = _event_text(event)
            if text:
                texts.append(text)
    if not texts:
        return output.strip()
    # Codex emits progress events and then the final message. Keep the last
    # meaningful event to avoid showing internal tool chatter in the UI.
    return texts[-1].strip()


class CommandProvider:
    def __init__(self, config: ProviderConfig):
        self.config = config

    def available(self) -> bool:
        command = shutil.which(self.config.command)
        return command is not None or Path(self.config.command).exists()

    def complete(self, message: str, context: dict[str, Any]) -> ProviderResult:
        prompt = _build_prompt(message, context)
        argv = _command_argv(self.config)
        stdin = prompt
        if self.config.prompt_mode == "argument":
            argv.append(prompt)
            stdin = None
        try:
            completed = subprocess.run(
                argv,
                input=stdin,
                text=True,
                capture_output=True,
                encoding="utf-8",
                errors="replace",
                timeout=self.config.timeout_seconds,
                cwd=self.config.cwd,
                check=False,
            )
        except FileNotFoundError as exc:
            raise ProviderError(f"{self.config.name} executable is not available") from exc
        except subprocess.TimeoutExpired as exc:
            raise ProviderError(f"{self.config.name} timed out") from exc
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout).strip()
            raise ProviderError(
                f"{self.config.name} exited with code {completed.returncode}: {detail[-1000:]}"
            )
        text = parse_provider_output(self.config.name, completed.stdout or "")
        if not text:
            raise ProviderError(f"{self.config.name} returned an empty response")
        return ProviderResult(self.config.name, text, completed.stdout)


def _build_prompt(message: str, context: dict[str, Any]) -> str:
    context_json = json.dumps(sanitize_context(context), ensure_ascii=False, sort_keys=True)
    return (
        "You are assisting with a test-tracking result. Use only the supplied context "
        "and clearly label assumptions. Do not execute SQL or modify files.\n\n"
        f"Result context (JSON):\n{context_json}\n\n"
        f"User question:\n{message.strip()}"
    )


class CodexProvider(CommandProvider):
    """Local Codex CLI adapter; configuration must include ``exec`` and ``-``."""

    def __init__(self, config: ProviderConfig):
        args = list(config.args)
        if not args or args[0] != "exec":
            args.insert(0, "exec")
        if "--json" not in args:
            args.append("--json")
        if "--sandbox" not in args:
            args.extend(["--sandbox", "read-only"])
        if "-" not in args:
            args.append("-")
        super().__init__(ProviderConfig(name=config.name, command=config.command,
                                        args=tuple(args), timeout_seconds=config.timeout_seconds,
                                        cwd=config.cwd, prompt_mode="stdin"))

    def complete(self, message: str, context: dict[str, Any]) -> ProviderResult:
        return super().complete(message, context)


class DshProvider(CommandProvider):
    """DSH headless profile adapter with a configurable executable and profile."""

    def __init__(self, config: ProviderConfig):
        args = list(config.args)
        if "--profile" not in args:
            args.extend(["--profile", "headless"])
        super().__init__(ProviderConfig(name=config.name, command=config.command,
                                        args=tuple(args), timeout_seconds=config.timeout_seconds,
                                        cwd=config.cwd, prompt_mode="argument"))

    def complete(self, message: str, context: dict[str, Any]) -> ProviderResult:
        return super().complete(message, context)


def build_providers(configs: dict[str, ProviderConfig]) -> dict[str, CommandProvider]:
    providers: dict[str, CommandProvider] = {}
    for name, value in configs.items():
        provider_type = name.lower()
        if provider_type == "codex":
            providers[name] = CodexProvider(value)
        elif provider_type == "dsh":
            providers[name] = DshProvider(value)
        else:
            providers[name] = CommandProvider(value)
    return providers
