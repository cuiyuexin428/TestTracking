from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class ProviderConfig:
    name: str
    command: str
    args: tuple[str, ...] = ()
    timeout_seconds: float = 120.0
    cwd: str | None = None
    prompt_mode: str = "stdin"

    @classmethod
    def from_dict(cls, name: str, value: dict[str, Any]) -> "ProviderConfig":
        command = str(value.get("command", name)).strip()
        raw_args = value.get("args", [])
        if not isinstance(raw_args, list) or not all(isinstance(arg, str) for arg in raw_args):
            raise ValueError(f"provider {name} args must be an array of strings")
        args = tuple(raw_args)
        timeout = float(value.get("timeout_seconds", 120.0))
        prompt_mode = str(value.get("prompt_mode", "stdin"))
        if prompt_mode not in {"stdin", "argument"}:
            raise ValueError(f"invalid prompt_mode for provider: {name}")
        if not command or timeout <= 0:
            raise ValueError(f"invalid provider configuration: {name}")
        return cls(name=name, command=command, args=args, timeout_seconds=timeout,
                   cwd=value.get("cwd"), prompt_mode=prompt_mode)


@dataclass(frozen=True)
class ServiceConfig:
    host: str = "127.0.0.1"
    port: int = 56330
    db_conn: str = ""
    stp_result_table: str | None = None
    workspace: str | None = None
    static_dir: str | None = None
    providers: dict[str, ProviderConfig] = field(default_factory=dict)

    @classmethod
    def defaults(cls) -> "ServiceConfig":
        return cls(
            providers={
                "codex": ProviderConfig(
                    name="codex",
                    command="codex",
                    # Keep the invocation compatible with the local Codex CLI,
                    # while leaving approval and sandbox policy to the user's
                    # own Codex configuration.
                    args=("exec", "--json", "--skip-git-repo-check", "-"),
                ),
                "dsh": ProviderConfig(
                    name="dsh",
                    command="dsh",
                    args=("--profile", "headless"),
                    prompt_mode="argument",
                ),
            }
        )

    @classmethod
    def from_file(cls, path: str | Path | None) -> "ServiceConfig":
        config = cls.defaults()
        if path:
            config_path = Path(path).resolve()
            with config_path.open("r", encoding="utf-8-sig") as handle:
                raw = json.load(handle)
            if not isinstance(raw, dict):
                raise ValueError("service configuration must be an object")
            config = cls._merge(config, raw)
            def resolved(value: str | None) -> str | None:
                return str((config_path.parent / value).resolve()) if value else None
            config = cls(
                host=config.host, port=config.port, db_conn=config.db_conn,
                stp_result_table=config.stp_result_table,
                workspace=resolved(config.workspace), static_dir=resolved(config.static_dir),
                providers={name: ProviderConfig(name=item.name, command=item.command, args=item.args,
                           timeout_seconds=item.timeout_seconds, cwd=resolved(item.cwd), prompt_mode=item.prompt_mode)
                           for name, item in config.providers.items()},
            )

        env = os.environ
        host = env.get("RESULT_SERVICE_HOST", config.host)
        port = int(env.get("RESULT_SERVICE_PORT", config.port))
        if not 0 <= port <= 65535:
            raise ValueError("port must be between 0 and 65535")
        db_conn = env.get("RESULT_SERVICE_DB", config.db_conn)
        workspace = env.get("RESULT_SERVICE_WORKSPACE", config.workspace or "") or None
        return cls(host=host, port=port, db_conn=db_conn,
                   stp_result_table=config.stp_result_table,
                   workspace=workspace, static_dir=config.static_dir,
                   providers=config.providers)

    @staticmethod
    def _merge(base: "ServiceConfig", raw: dict[str, Any]) -> "ServiceConfig":
        providers = dict(base.providers)
        for name, value in raw.get("providers", {}).items():
            if not isinstance(value, dict):
                raise ValueError(f"provider {name} must be an object")
            merged_value = dict(value)
            if "args" not in merged_value and name in providers:
                merged_value["args"] = list(providers[name].args)
            if "prompt_mode" not in merged_value and name.lower() == "dsh":
                merged_value["prompt_mode"] = "argument"
            providers[name] = ProviderConfig.from_dict(name, merged_value)
        return ServiceConfig(
            host=str(raw.get("host", base.host)),
            port=int(raw.get("port", base.port)),
            db_conn=str(raw.get("db_conn", base.db_conn)),
            stp_result_table=raw.get("stp_result_table", base.stp_result_table),
            workspace=raw.get("workspace", base.workspace),
            static_dir=raw.get("static_dir", base.static_dir),
            providers=providers,
        )
