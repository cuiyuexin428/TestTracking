"""Persistent desired and reported deployment state for client agents."""
from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .artifacts import ArtifactStore
from .program_bindings import bind_programs


_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
_ARTIFACT_ID = re.compile(r"[0-9a-f]{32}")
_REVISION = re.compile(r"[0-9a-f]{64}")
_DESIRED_LIMIT = 256
_HISTORY_LIMIT = 64
_VISIBLE_HISTORY_LIMIT = 10
_ARTIFACT_REFERENCE = re.compile(r"\{artifact:([A-Za-z0-9][A-Za-z0-9._-]{0,127})\}(?:[/\\](.*))?")
_REPORT_STATES = frozenset({"waiting_window", "waiting_idle", "applying", "staged", "ready", "failed"})
_COMPONENT_STATES = frozenset({"pending", "downloading", "staged", "active", "failed"})


class DeploymentError(RuntimeError):
    def __init__(self, message: str, status: int = 400, *, current_revision: str | None = None):
        super().__init__(message)
        self.status = status
        self.current_revision = current_revision


class DeploymentStore:
    """Atomically store client deployment intent and the latest agent report."""

    def __init__(self, path: str | Path, artifacts: ArtifactStore):
        self.path = Path(path).resolve()
        self.artifacts = artifacts
        self._lock = threading.RLock()
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def list(self) -> list[dict[str, Any]]:
        with self._lock:
            clients = self._read()["clients"]
            return [self._expand(client_id, value)
                    for client_id, value in sorted(clients.items())]

    def get(self, client_id: Any) -> dict[str, Any]:
        client = self._client_id(client_id)
        with self._lock:
            value = self._read()["clients"].get(client, {})
            return self._expand(client, value)

    def put_desired(self, client_id: Any, body: Any) -> tuple[dict[str, Any], bool]:
        client = self._client_id(client_id)
        if not isinstance(body, dict):
            raise DeploymentError("request body must be an object")
        if "revision" not in body:
            raise DeploymentError("revision is required and must be null for the first update")
        expected = self._optional_revision(body.get("revision"), "revision")
        action = body.get("action", "deploy")
        if action not in ("deploy", "rollback"):
            raise DeploymentError("action must be deploy or rollback")
        rollback_revision = None
        if action == "rollback":
            rollback_revision = self._required_revision(body.get("target_revision"), "target_revision")
            if "components" in body or "config_patch" in body:
                raise DeploymentError("rollback uses the saved components and config_patch")
            components, config_patch = [], {}
        else:
            components = self._plan_components(body.get("components"))
            config_patch = self.prepare(components, body.get("config_patch", {}))["config_patch"]
        updated_by = self._optional_identifier(body.get("operator_id"), "operator_id")
        reason = self._optional_text(body.get("reason"), "reason", 4096)
        with self._lock:
            state = self._read()
            entry = state["clients"].setdefault(client, {})
            current = entry.get("desired")
            current_revision = current.get("revision") if current else None
            if (action == "rollback" and current
                    and current.get("rollback_of") == rollback_revision
                    and current.get("previous_revision") == expected):
                return self._expand(client, entry), False
            if (action == "deploy" and current and current.get("components") == components
                    and json.dumps(current.get("config_patch", {}), sort_keys=True)
                    == json.dumps(config_patch, sort_keys=True)):
                return self._expand(client, entry), False
            if expected != current_revision:
                raise DeploymentError(
                    "desired deployment changed; reload it before saving", 409,
                    current_revision=current_revision,
                )
            if action == "rollback":
                historical = next((item for item in reversed(entry.get("desired_history", []))
                                   if item.get("revision") == rollback_revision), None)
                if historical is None:
                    raise DeploymentError("rollback target_revision was not found for this client", 404)
                if rollback_revision == current_revision:
                    raise DeploymentError("rollback target_revision must be a previous deployment")
                components = self._plan_components(historical.get("components"))
                config_patch = self.prepare(components, historical.get("config_patch", {}))["config_patch"]
            revision = self._next_revision(current_revision, components, config_patch)
            desired = {
                "revision": revision,
                "previous_revision": current_revision,
                "components": components,
                "config_patch": config_patch,
                "updated_at": self._now(),
                "updated_by": updated_by,
                "reason": reason,
                "action": action,
                "rollback_of": rollback_revision,
            }
            entry["desired"] = desired
            history = entry.setdefault("desired_history", [])
            history.append(desired)
            del history[:-_HISTORY_LIMIT]
            self._write(state)
            return self._expand(client, entry), True

    def put_reported(self, client_id: Any, body: Any) -> tuple[dict[str, Any], bool]:
        client = self._client_id(client_id)
        if not isinstance(body, dict):
            raise DeploymentError("request body must be an object")
        desired_revision = self._required_revision(body.get("desired_revision"), "desired_revision")
        report_state = body.get("state")
        if not isinstance(report_state, str) or report_state not in _REPORT_STATES:
            raise DeploymentError("state must be waiting_window, waiting_idle, applying, staged, ready, or failed")
        components = self._reported_components(body.get("components"))
        if report_state == "ready" and any(item["state"] != "active" for item in components):
            raise DeploymentError("every component must be active when state is ready")
        agent_version = self._optional_text(body.get("agent_version"), "agent_version", 128)
        message = self._optional_text(body.get("message"), "message", 4096)
        report_revision = hashlib.sha256(json.dumps({
            "desired_revision": desired_revision,
            "state": report_state,
            "components": components,
            "agent_version": agent_version,
            "message": message,
        }, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")).hexdigest()

        with self._lock:
            state = self._read()
            entry = state["clients"].setdefault(client, {})
            known = next((item for item in reversed(entry.get("desired_history", []))
                          if item.get("revision") == desired_revision), None)
            if known is None:
                current_revision = entry.get("desired", {}).get("revision")
                raise DeploymentError(
                    "desired_revision is not known for this client", 409,
                    current_revision=current_revision,
                )
            current_desired_revision = entry.get("desired", {}).get("revision")
            if desired_revision != current_desired_revision:
                # A delayed report from an earlier generation must not roll the
                # visible reported state back after a newer deployment was set.
                raise DeploymentError(
                    "desired_revision is no longer current", 409,
                    current_revision=current_desired_revision,
                )
            reported_plan = [{"slot": item["slot"], "artifact_id": item["artifact_id"]}
                             for item in components]
            if reported_plan != known.get("components"):
                raise DeploymentError(
                    "reported components do not match the referenced desired deployment"
                )
            current = entry.get("reported")
            if current and current.get("revision") == report_revision:
                return self._expand(client, entry), False
            entry["reported"] = {
                "revision": report_revision,
                "desired_revision": desired_revision,
                "state": report_state,
                "components": components,
                "agent_version": agent_version,
                "message": message,
                "updated_at": self._now(),
            }
            self._write(state)
            return self._expand(client, entry), True

    def _expand(self, client_id: str, value: dict[str, Any]) -> dict[str, Any]:
        desired = self._expand_state(value.get("desired"))
        reported = self._expand_state(value.get("reported"))
        if desired is None:
            status = "unconfigured"
        elif reported is None or reported.get("desired_revision") != desired["revision"]:
            status = "pending"
        elif reported["state"] == "ready":
            status = "current"
        else:
            status = reported["state"]
        previous = [item for item in reversed(value.get("desired_history", []))
                    if desired is None or item.get("revision") != desired["revision"]]
        history = [self._expand_state(item) for item in previous[:_VISIBLE_HISTORY_LIMIT]]
        return {"client_id": client_id, "state": status, "desired": desired,
                "reported": reported, "history": history}

    def _expand_state(self, value: Any) -> dict[str, Any] | None:
        if value is None:
            return None
        if not isinstance(value, dict) or not isinstance(value.get("components"), list):
            raise DeploymentError("deployment state has an unsupported format", 500)
        try:
            expanded = dict(value)
            if "desired_revision" not in value:
                expanded.setdefault("config_patch", {})
                canonical, bindings, expected = bind_programs(
                    self.artifacts, value["components"], expanded["config_patch"], validate_files=False)
                # Do not relabel old on-disk desired configurations as bound.
                expanded["program_bindings"] = bindings if canonical == expanded["config_patch"] else []
                expanded["scheduler_plugins"] = expected if canonical == expanded["config_patch"] else {}
            expanded["components"] = []
            for component in value["components"]:
                item = dict(component)
                artifact = self.artifacts.get(item["artifact_id"])
                if artifact is None:
                    raise DeploymentError(
                        f"deployment references missing artifact: {item['artifact_id']}", 500
                    )
                item["artifact"] = artifact
                expanded["components"].append(item)
            return expanded
        except (KeyError, TypeError, ValueError) as exc:
            raise DeploymentError("deployment state has an unsupported format", 500) from exc

    def prepare(self, components: Any, config_patch: Any) -> dict[str, Any]:
        plan = self._plan_components(components)
        patch = self._config_patch(config_patch, plan)
        patch, bindings, expected = bind_programs(self.artifacts, plan, patch)
        return {"components": plan, "config_patch": patch,
                "program_bindings": bindings, "scheduler_plugins": expected}

    def _plan_components(self, value: Any) -> list[dict[str, str]]:
        if not isinstance(value, list):
            raise DeploymentError("components must be an array")
        if len(value) > _DESIRED_LIMIT:
            raise DeploymentError(f"components cannot contain more than {_DESIRED_LIMIT} entries")
        result: list[dict[str, str]] = []
        slots: set[str] = set()
        for component in value:
            if not isinstance(component, dict):
                raise DeploymentError("each component must be an object")
            slot = self._identifier(component.get("slot"), "component slot")
            artifact_id = component.get("artifact_id")
            if not isinstance(artifact_id, str) or not _ARTIFACT_ID.fullmatch(artifact_id):
                raise DeploymentError("component artifact_id must be a lowercase artifact ID")
            if slot in slots:
                raise DeploymentError(f"component slot is duplicated: {slot}")
            if self.artifacts.get(artifact_id) is None:
                raise DeploymentError(f"artifact not found: {artifact_id}", 404)
            slots.add(slot)
            result.append({"slot": slot, "artifact_id": artifact_id})
        return sorted(result, key=lambda item: item["slot"])

    def _reported_components(self, value: Any) -> list[dict[str, Any]]:
        plan = self._plan_components(value)
        originals = {item.get("slot"): item for item in value}
        result: list[dict[str, Any]] = []
        for item in plan:
            original = originals[item["slot"]]
            component_state = original.get("state")
            if not isinstance(component_state, str) or component_state not in _COMPONENT_STATES:
                raise DeploymentError(
                    "component state must be pending, downloading, staged, active, or failed"
                )
            error = self._optional_text(original.get("error"), "component error", 2048)
            result.append({**item, "state": component_state, "error": error})
        return result

    @staticmethod
    def _next_revision(previous: str | None, components: list[dict[str, str]],
                       config_patch: dict[str, Any]) -> str:
        canonical = json.dumps(
            {"previous_revision": previous, "components": components, "config_patch": config_patch},
            sort_keys=True, separators=(",", ":"),
        ).encode("utf-8")
        return hashlib.sha256(canonical).hexdigest()

    @staticmethod
    def _config_patch(value: Any, components: list[dict[str, str]]) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise DeploymentError("config_patch must be an object")
        if "client" in value:
            client_patch = value["client"]
            if not isinstance(client_patch, dict):
                raise DeploymentError("config_patch.client must be an object")
            protected = {"worker_id", "deployment_endpoint"}.intersection(client_patch)
            if protected:
                raise DeploymentError(
                    "config_patch cannot change client." + sorted(protected)[0]
                )
        plugins_patch = value.get("plugins")
        if isinstance(plugins_patch, dict) and sum(
            isinstance(plugin, dict) and plugin.get("enabled") is True
            for plugin in plugins_patch.values()
        ) > 1:
            # Only the client has its base configuration. It validates the final
            # merged count, including plugins omitted from this partial patch.
            raise DeploymentError("Client deployment supports only one enabled plugin")
        slots = {item["slot"] for item in components}

        def validate(item: Any) -> None:
            if isinstance(item, dict):
                for key, child in item.items():
                    if not isinstance(key, str):
                        raise DeploymentError("config_patch object keys must be strings")
                    validate(child)
            elif isinstance(item, list):
                for child in item:
                    validate(child)
            elif isinstance(item, str) and "{artifact:" in item:
                reference = _ARTIFACT_REFERENCE.fullmatch(item)
                if reference is None:
                    raise DeploymentError("artifact path must use {artifact:slot}/relative/path")
                slot, relative = reference.groups()
                if slot not in slots:
                    raise DeploymentError(f"config_patch references unknown artifact slot: {slot}")
                if relative and (".." in re.split(r"[/\\]", relative)
                                 or relative.startswith(("/", "\\")) or ":" in relative):
                    raise DeploymentError("artifact path must remain inside its artifact directory")

        try:
            validate(value)
            encoded = json.dumps(value, ensure_ascii=False, allow_nan=False)
            if len(encoded.encode("utf-8")) > 128 * 1024:
                raise DeploymentError("config_patch cannot exceed 128 KiB")
            return json.loads(encoded)
        except (TypeError, ValueError, RecursionError) as exc:
            raise DeploymentError("config_patch must contain valid JSON values") from exc

    def _read(self) -> dict[str, Any]:
        if not self.path.exists():
            return {"schema_version": 1, "clients": {}}
        try:
            value = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError, UnicodeError) as exc:
            raise DeploymentError("deployment state cannot be read", 500) from exc
        if (not isinstance(value, dict) or value.get("schema_version") != 1
                or not isinstance(value.get("clients"), dict)):
            raise DeploymentError("deployment state has an unsupported format", 500)
        return value

    def _write(self, state: dict[str, Any]) -> None:
        data = (json.dumps(state, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        descriptor, name = tempfile.mkstemp(prefix="deployments-", suffix=".tmp", dir=self.path.parent)
        temporary = Path(name)
        try:
            with os.fdopen(descriptor, "wb") as output:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.path)
        finally:
            temporary.unlink(missing_ok=True)

    @staticmethod
    def _now() -> str:
        return datetime.now(timezone.utc).isoformat()

    @staticmethod
    def _client_id(value: Any) -> str:
        return DeploymentStore._identifier(value, "client_id")

    @staticmethod
    def _identifier(value: Any, field: str) -> str:
        if not isinstance(value, str) or not _IDENTIFIER.fullmatch(value):
            raise DeploymentError(
                f"{field} must use 1-128 ASCII letters, digits, dots, underscores, or hyphens"
            )
        return value

    @staticmethod
    def _optional_identifier(value: Any, field: str) -> str | None:
        if value is None:
            return None
        return DeploymentStore._identifier(value, field)

    @staticmethod
    def _required_revision(value: Any, field: str) -> str:
        if not isinstance(value, str) or not _REVISION.fullmatch(value):
            raise DeploymentError(f"{field} must be a lowercase SHA-256 revision")
        return value

    @staticmethod
    def _optional_revision(value: Any, field: str) -> str | None:
        if value is None:
            return None
        return DeploymentStore._required_revision(value, field)

    @staticmethod
    def _optional_text(value: Any, field: str, maximum: int) -> str | None:
        if value is None:
            return None
        if not isinstance(value, str) or len(value.encode("utf-8")) > maximum:
            raise DeploymentError(f"{field} must be a string of at most {maximum} UTF-8 bytes")
        return value
