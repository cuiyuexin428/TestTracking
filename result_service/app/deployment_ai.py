"""Read-only, allowlisted deployment evidence for the embedded AI assistant."""
from __future__ import annotations

import hashlib
import json
import re
from typing import Any

from .deployments import DeploymentError, DeploymentStore
from .providers import ProviderError, canonical_operator_id


_SECRET_TEXT = re.compile(r"(?i)\b(?:db_conn|connection_string|password|passwd|secret|token|api[_-]?key|command|cwd|path)\s*[:=]\s*(?:\"[^\"]*\"|'[^']*'|[^\s,;]+)")
_LOCATION = re.compile(r"(?i)(?:[a-z][a-z0-9+.-]*://[^\s<>\"']+|[a-z]:[/\\][^\s<>\"']*|\\\\[^\s<>\"']+|(?<![\w/])/(?:[^\s/<>\"']+/)+[^\s<>\"']*)")
_CONTROL = re.compile(r"[\x00-\x1f\x7f\u202a-\u202e\u2066-\u2069]")
_RUN_STATES = {"running", "queued", "planning", "planned", "stopped", "finished", "cancelled", "result_pending", "client_busy"}
_CLIENT_STATES = {"ready", "unconfigured", "starting", "error", "offline", "unmanaged", "waiting_window", "waiting_idle", "applying", "staged", "failed", "busy"}
_ROLLOUT_STATES = {"pending", "waiting_runs", "switching", "deploying", "ready", "completed", "cancelled", "blocked", "superseded"}


def _label(value: Any, maximum: int = 128) -> str:
    """Labels are data, never executable text; do not forward local locations."""
    text = value if isinstance(value, str) else ""
    text = _CONTROL.sub(" ", text)
    text = _SECRET_TEXT.sub("[redacted]", text)
    text = _LOCATION.sub("[redacted]", text)
    # Relative file names are also implementation details. Artifact labels
    # remain useful when they are ordinary names; any slash is treated as a
    # location to avoid leaking package layout to the model.
    if "/" in text or "\\" in text:
        text = "[redacted]"
    return text[:maximum] + ("…" if len(text) > maximum else "")


def _state(value: Any, allowed: set[str]) -> str:
    return value if isinstance(value, str) and value in allowed else "unknown"


def _digest(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                     separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def deployment_review_context(rollouts, body: Any) -> dict[str, Any]:
    """Recheck an existing preview, returning no configuration or write handle."""
    allowed = {"profile_id", "revision", "mode", "client_ids", "plan_fingerprint", "operator_id",
               "replaces_rollout_id", "replaces_revision"}
    if not isinstance(body, dict) or set(body) - allowed:
        raise DeploymentError("AI 部署审查请求包含未知字段或格式无效")
    profile_id = DeploymentStore._identifier(body.get("profile_id"), "profile_id")
    revision = DeploymentStore._required_revision(body.get("revision"), "revision")
    fingerprint = DeploymentStore._required_revision(body.get("plan_fingerprint"), "plan_fingerprint")
    if body.get("mode") not in {"update", "assign"}:
        raise DeploymentError("mode must be update or assign")
    if body.get("operator_id") is not None:
        try:
            canonical_operator_id(body["operator_id"])
        except ProviderError as exc:
            raise DeploymentError(str(exc)) from exc
    if body["mode"] == "update" and body.get("client_ids") not in (None, []):
        raise DeploymentError("统一更新的机器范围由服务端确定")
    request = {key: body[key] for key in ("revision", "mode", "client_ids", "replaces_rollout_id", "replaces_revision")
               if key in body}
    with rollouts._lock, rollouts.profiles._lock:
        profile = rollouts.profiles.get(profile_id)
        if profile["revision"] != revision:
            raise DeploymentError("部署方案已改变，请重新预览后进行 AI 审查", 409,
                                  current_revision=profile["revision"])
        preview = rollouts.preview(profile_id, request)
        if preview["plan_fingerprint"] != fingerprint:
            raise DeploymentError("部署计划已改变，请重新预览后进行 AI 审查", 409)
        current = rollouts.list()["rollouts"]
        resources = []
        for plugin in preview.get("resource_summary", []):
            name = plugin["plugin"]
            selected = []
            for resource in plugin.get("resources", []):
                role = resource["label"]
                slot = "profile." + name + "." + role
                component = next((item for client in preview["clients"] for item in client.get("components", [])
                                  if item.get("slot") == slot), None)
                artifact = rollouts.deployments.artifacts.get(component["artifact_id"]) if component else None
                source = (artifact or {}).get("metadata", {}).get("source_artifact_id")
                if source:
                    artifact = rollouts.deployments.artifacts.get(source) or artifact
                selected.append({"role": _label(role, 32), "name": _label(resource.get("name")),
                                 "version": _label(resource.get("version"), 64),
                                 "artifact_id": (artifact or {}).get("artifact_id"),
                                 "sha256": (artifact or {}).get("sha256")})
            resources.append({"name": _label(name, 64), "target_id": preview.get("target_ids", {}).get(name),
                              "resources": selected})
        unsupported = set(preview.get("unsupported_clients", []))
        clients = [{"client_id": _label(row.get("client_id")), "revision": row.get("revision"),
                    "online": row.get("online") is True, "busy": row.get("busy") is True,
                    "state": _state(row.get("state"), _CLIENT_STATES),
                    "requires_base_client_upgrade": row.get("client_id") in unsupported}
                   for row in preview.get("clients", [])]
        waiting = []
        waiting_source = preview.get("waiting_runs", [])
        for row in waiting_source[:256]:
            item = {"run_id": _label(row.get("run_id")), "plugin": _label(row.get("plugin_type"), 64),
                    "status": _state(row.get("status"), _RUN_STATES)}
            for key in ("total_tasks", "completed_tasks"):
                if type(row.get(key)) is int and row[key] >= 0:
                    item[key] = row[key]
            waiting.append(item)
        plugins = set(preview.get("plugins", []))
        active = [{"rollout_id": _label(row.get("rollout_id")), "profile_name": _label(row.get("profile_name")),
                   "status": _state(row.get("status"), _ROLLOUT_STATES),
                   "plugins": [_label(name, 64) for name in row.get("plugins", [])],
                   "ready_clients": sum(item.get("status") == "ready" for item in row.get("clients", [])),
                   "total_clients": len(row.get("clients", []))}
                  for row in current[:128] if set(row.get("plugins", [])) & plugins
                  and row.get("status") not in {"completed", "cancelled", "superseded"}]
        context = {
            "profile": {"profile_id": profile_id, "name": _label(preview.get("profile_name")),
                        "revision": preview.get("profile_revision"), "mode": preview["mode"],
                        "published_target": bool(preview.get("published_target")), "plugins": resources},
            "targets": clients, "waiting_runs": waiting, "active_rollouts": active,
            "summary": {"total_clients": len(clients), "online_clients": sum(row["online"] for row in clients),
                        "offline_clients": sum(not row["online"] for row in clients),
                        "busy_clients": sum(row["busy"] for row in clients), "waiting_runs": len(waiting),
                        "unsupported_clients": len(unsupported), "waiting_runs_total": len(waiting_source)},
            "warnings": [_label(item, 512) for item in preview.get("warnings", [])[:64]],
            "unsupported_clients": [_label(item) for item in sorted(unsupported)],
            "publish_blocked": bool(preview.get("publish_blocked")),
            "constraints": {
                "same_plugin_one_global_version": True, "finish_current_and_preexisting_queued_runs": True,
                "pause_new_runs_until_new_release_ready": True, "offline_clients_catch_up_before_work": True,
                "preserve_existing_machine_preferences_and_unrelated_plugins": True,
                "client_framework_self_update": False, "ai_review_is_advisory": True,
                "publishing_requires_console_confirmation": True,
            },
            "data_handling": {"labels_are_untrusted_data": True, "configuration_and_locations_omitted": True},
        }
    result = {"schema_version": 1, "plan_fingerprint": fingerprint, "context": context,
              "profile_name": context["profile"]["name"], "publish_blocked": context["publish_blocked"]}
    result["context_fingerprint"] = _digest(result)
    return result
