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
    host: str = "0.0.0.0"
    port: int = 56330
    db_conn: str = ""
    stp_result_table: str | None = None
    workspace: str | None = None
    static_dir: str | None = None
    plugin_views_dir: str | None = None
    plugin_adapters_dir: str | None = None
    plugin_ai_contexts_dir: str | None = None
    analysis_scripts_dir: str | None = None
    analysis_reports_dir: str | None = None
    report_templates_dir: str | None = None
    artifact_store_dir: str | None = None
    artifact_import_roots: tuple[str, ...] = ()
    scheduler_management_url: str = "http://127.0.0.1:5556"
    scheduler_config_path: str | None = None
    client_config_path: str | None = None
    logs_dir: str | None = None
    config_path: str | None = None
    plugin_options: dict[str, dict[str, Any]] = field(default_factory=dict)
    providers: dict[str, ProviderConfig] = field(default_factory=dict)

    @classmethod
    def defaults(cls) -> "ServiceConfig":
        return cls(
            providers={
                "dsh": ProviderConfig(
                    name="dsh",
                    command="dsh",
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
                plugin_views_dir=resolved(config.plugin_views_dir),
                plugin_adapters_dir=resolved(config.plugin_adapters_dir),
                plugin_ai_contexts_dir=resolved(config.plugin_ai_contexts_dir),
                analysis_scripts_dir=resolved(config.analysis_scripts_dir),
                analysis_reports_dir=resolved(config.analysis_reports_dir),
                report_templates_dir=resolved(config.report_templates_dir),
                artifact_store_dir=resolved(config.artifact_store_dir),
                artifact_import_roots=tuple(
                    str((config_path.parent / value).resolve())
                    for value in config.artifact_import_roots
                ),
                scheduler_management_url=config.scheduler_management_url,
                scheduler_config_path=resolved(config.scheduler_config_path),
                client_config_path=resolved(config.client_config_path),
                logs_dir=resolved(config.logs_dir), config_path=str(config_path),
                plugin_options=config.plugin_options,
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
        plugin_views_dir = env.get(
            "RESULT_SERVICE_PLUGIN_VIEWS", config.plugin_views_dir or ""
        ) or None
        plugin_adapters_dir = env.get(
            "RESULT_SERVICE_PLUGIN_ADAPTERS", config.plugin_adapters_dir or ""
        ) or None
        plugin_ai_contexts_dir = env.get(
            "RESULT_SERVICE_PLUGIN_AI_CONTEXTS", config.plugin_ai_contexts_dir or ""
        ) or None
        return cls(host=host, port=port, db_conn=db_conn,
                   stp_result_table=config.stp_result_table,
                   workspace=workspace, static_dir=config.static_dir,
                   plugin_views_dir=plugin_views_dir,
                   plugin_adapters_dir=plugin_adapters_dir,
                   plugin_ai_contexts_dir=plugin_ai_contexts_dir,
                   analysis_scripts_dir=config.analysis_scripts_dir,
                   analysis_reports_dir=config.analysis_reports_dir,
                   report_templates_dir=config.report_templates_dir,
                   artifact_store_dir=config.artifact_store_dir,
                   artifact_import_roots=config.artifact_import_roots,
                   scheduler_management_url=config.scheduler_management_url,
                   scheduler_config_path=config.scheduler_config_path,
                   client_config_path=config.client_config_path,
                   logs_dir=config.logs_dir, config_path=config.config_path,
                   plugin_options=config.plugin_options,
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
        plugin_options = raw.get("plugin_options", base.plugin_options)
        if not isinstance(plugin_options, dict) or not all(
                isinstance(name, str) and isinstance(value, dict)
                for name, value in plugin_options.items()):
            raise ValueError("plugin_options must map plugin names to objects")
        import_roots = raw.get("artifact_import_roots", base.artifact_import_roots)
        if (not isinstance(import_roots, (list, tuple))
                or not all(isinstance(value, str) and value.strip() for value in import_roots)):
            raise ValueError("artifact_import_roots must be an array of nonempty paths")
        return ServiceConfig(
            host=str(raw.get("host", base.host)),
            port=int(raw.get("port", base.port)),
            db_conn=str(raw.get("db_conn", base.db_conn)),
            stp_result_table=raw.get("stp_result_table", base.stp_result_table),
            workspace=raw.get("workspace", base.workspace),
            static_dir=raw.get("static_dir", base.static_dir),
            plugin_views_dir=raw.get("plugin_views_dir", base.plugin_views_dir),
            plugin_adapters_dir=raw.get("plugin_adapters_dir", base.plugin_adapters_dir),
            plugin_ai_contexts_dir=raw.get(
                "plugin_ai_contexts_dir", base.plugin_ai_contexts_dir
            ),
            analysis_scripts_dir=raw.get("analysis_scripts_dir", base.analysis_scripts_dir),
            analysis_reports_dir=raw.get("analysis_reports_dir", base.analysis_reports_dir),
            report_templates_dir=raw.get("report_templates_dir", base.report_templates_dir),
            artifact_store_dir=raw.get("artifact_store_dir", base.artifact_store_dir),
            artifact_import_roots=tuple(import_roots),
            scheduler_management_url=raw.get("scheduler_management_url", base.scheduler_management_url),
            scheduler_config_path=raw.get("scheduler_config_path", base.scheduler_config_path),
            client_config_path=raw.get("client_config_path", base.client_config_path),
            logs_dir=raw.get("logs_dir", base.logs_dir), config_path=base.config_path,
            plugin_options=plugin_options,
            providers=providers,
        )
