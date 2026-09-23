"""Durable plugin-wide deployment coordination, independent of browser lifetime."""
from __future__ import annotations

import copy
import hashlib
import json
import logging
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .deployments import DeploymentError, DeploymentStore, _ARTIFACT_REFERENCE
from .fleet import merge_config
from .management import ManagementError, _masked
from .resource_catalog import atomic_write, json_bytes

LOG = logging.getLogger("result_service.rollouts")
TERMINAL = {"completed", "cancelled", "superseded"}
CAPABILITY = "deployment_target_id_v1"


def digest(value: Any) -> str:
    return hashlib.sha256(json_bytes(value)).hexdigest()


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


class DeploymentRollouts:
    def __init__(self, path: Path, profiles, fleet, *, start: bool = False):
        self.path = Path(path).resolve()
        self.profiles, self.fleet = profiles, fleet
        self.deployments = fleet.deployments
        # Rollout progression performs management HTTP calls.  Keep its state
        # lock private so a slow coordinator tick cannot hold ClientFleet's
        # lock and stall agent discovery or heartbeat bookkeeping.
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._thread = None
        self.fleet.rollout_guard = self.guard_advanced
        self.fleet.rollout_delivery_guard = self.guard_delivery
        if start:
            self.start()

    @property
    def management(self):
        # Tests and embedded services may replace their management connection.
        return self.fleet.management

    def start(self):
        if self._thread is None:
            self._thread = threading.Thread(target=self._run, name="deployment-rollouts", daemon=True)
            self._thread.start()

    def close(self):
        self._stop.set()
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=40)

    def _run(self):
        while not self._stop.wait(.5):
            try:
                self.tick()
            except Exception:
                LOG.exception("Deployment coordinator will retry after failure")

    def _read(self):
        if not self.path.exists():
            return {"schema_version": 1, "requests": {}, "targets": {}}
        try:
            value = json.loads(self.path.read_text(encoding="utf-8"))
            if value.get("schema_version") != 1 or not isinstance(value["requests"], dict) or not isinstance(value["targets"], dict):
                raise ValueError("invalid rollout store")
            return value
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise DeploymentError("部署协调记录无法读取，请保留文件并检查服务端存储", 500) from error

    def _write(self, state):
        atomic_write(self.path, json_bytes(state))

    def _save(self, state, record):
        value = {key: item for key, item in record.items() if key not in {"revision", "updated_at"}}
        revision = digest(value)
        if record.get("revision") != revision:
            record.update(revision=revision, updated_at=now())
        state["requests"][record["rollout_id"]] = record
        self._write(state)

    @staticmethod
    def _public(record):
        keys = ("rollout_id", "revision", "profile_id", "profile_name", "profile_revision", "mode",
                "plugins", "target_ids", "waiting_runs", "created_at", "updated_at", "error", "resource_summary",
                "published_target")
        result = {key: copy.deepcopy(record[key]) for key in keys if key in record}
        result["status"] = "blocked" if record.get("error") and record["stage"] not in TERMINAL else record["stage"]
        result["phase"] = record["stage"]
        result["clients"] = [{key: copy.deepcopy(value) for key, value in item.items()
                              if key in {"client_id", "status", "error", "revision", "plugins", "online"}}
                             for item in record["clients"].values()]
        result["can_cancel"] = (record["stage"] in {"pending", "waiting_runs"}
                                and not record.get("cancel_requested") and not record.get("replaces_rollout_id"))
        result["can_retry"] = record["stage"] not in TERMINAL and (
            any(item["status"] == "failed" for item in record["clients"].values()) or bool(record.get("error")))
        return result

    def list(self):
        with self._lock:
            state = self._read()
            targets = {name: {key: value for key, value in target.items()
                        if key in {"target_id", "profile_id", "profile_name", "rollout_id", "ready"}}
                       for name, target in state["targets"].items()}
            return {"rollouts": [self._public(item) for item in sorted(state["requests"].values(),
                        key=lambda item: item["created_at"], reverse=True)], "targets": targets}

    def get(self, rollout_id):
        with self._lock:
            return self._public(self._record(self._read(), rollout_id))

    @staticmethod
    def _record(state, rollout_id):
        value = state["requests"].get(rollout_id)
        if value is None:
            raise DeploymentError("部署更新请求不存在", 404)
        return value

    @staticmethod
    def _pending(state, plugins, ignore=None):
        return [item for item in state["requests"].values() if item["rollout_id"] != ignore
                and item["stage"] not in TERMINAL | {"ready"}
                and set(item["plugins"]) & set(plugins)]

    def blocks(self):
        # The store is replaced atomically.  Status polling only needs a
        # coherent snapshot and must remain responsive while tick performs
        # management HTTP calls under its private state lock.
        state = self._read()
        return {name: {"rollout_id": item["rollout_id"], "reason": item.get("error") or "插件更新正在等待旧批次完成或新版机器就绪"}
                for item in state["requests"].values() if item["stage"] not in TERMINAL | {"ready"}
                for name in item["plugins"] if name in item["changing"]}

    def guard_run(self, body):
        if isinstance(body, dict) and body.get("action") == "run" and body.get("plugin_type") in self.blocks():
            raise ManagementError("该插件正在更新，请等待新版可运行后再提交任务", 409)

    def _release_window(self, client, value, row):
        desired = value["desired"] or {}
        if (value["state"] == "waiting_window" and row.get("state") in {"ready", "unconfigured", "starting", "error"}
                and not row.get("busy")):
            self.management.client_control("release_clients", client_ids=[client], reservation_id=desired["reservation_id"])

    def guard_delivery(self, client, value):
        token = (value["desired"] or {}).get("reservation_id")
        managed = any(item.get("token") == token for record in self._read()["requests"].values()
                      for item in record["clients"].values())
        if not managed:
            return False
        row = next((row for row in self.management.get_status().get("agents", []) if row["client_id"] == client), {})
        if row.get("update_window_open") is False:
            # A helper that has not seen this generation must first receive it
            # under the existing reservation and acknowledge waiting_window.
            # Once acknowledged, never deliver it again without reacquiring.
            if value["state"] != "waiting_window" and row.get("reservation_id") == token:
                return True
            self._release_window(client, value, row)
            raise ManagementError("等待客户端更新时间窗，当前不会占用机器", 409)
        return True

    def guard_advanced(self, body, *, client=None):
        state = self._read()
        managed = set(state["targets"]) | {name for item in state["requests"].values()
                    if item["stage"] != "cancelled" for name in item["plugins"]}
        if not managed:
            return
        patch = body.get("config_patch", {}) if isinstance(body, dict) else {}
        affected = set(patch.get("plugins", {})) if isinstance(patch, dict) else set()
        ids = [client] if client else [item.get("client_id") for item in body.get("targets", [])]
        if not ids and affected & managed:
            raise DeploymentError("该插件由统一部署目标管理，请使用部署更新或机器分配入口", 409)

        def resource_config(value, slots):
            if isinstance(value, dict):
                return {key: resource_config(item, slots) for key, item in value.items()}
            if isinstance(value, list):
                return [resource_config(item, slots) for item in value]
            if isinstance(value, str):
                match = _ARTIFACT_REFERENCE.fullmatch(value)
                if match:
                    return {"artifact_id": slots.get(match[1]), "path": match[2] or ""}
            return value

        for identifier in ids:
            if not identifier:
                continue
            current = self.deployments.get(identifier)["desired"] or {}
            original = current.get("config_patch", {})
            components = current.get("components", [])
            if client and body.get("action") == "rollback":
                previous = self.deployments.historical(client, body.get("target_revision"))
                incoming, incoming_components = previous["config_patch"], previous["components"]
            else:
                incoming = patch if client else merge_config(original, patch)
                incoming_components = (body.get("components", []) if client else components + body.get("components", []))
            old_slots = {item["slot"]: item["artifact_id"] for item in components}
            new_slots = {item["slot"]: item["artifact_id"] for item in incoming_components}
            if any(resource_config(original.get("plugins", {}).get(name), old_slots)
                   != resource_config(incoming.get("plugins", {}).get(name), new_slots) for name in managed):
                raise DeploymentError("统一部署的插件资源不能被高级操作覆盖；机器时间窗和并发可独立调整", 409)

    def guard_config(self, kind, body):
        if kind != "scheduler" or not isinstance(body, dict):
            return
        state = self._read()
        managed = set(state["targets"]) | {name for item in state["requests"].values()
                   if item["stage"] not in TERMINAL for name in item["plugins"]}
        mapping = self.management.get_status().get("plugin_config_keys", {})
        current = self.management.get_config("scheduler").get("config", {}).get("plugins", {})
        incoming = body.get("config", {}).get("plugins", {})
        if any(incoming.get(mapping.get(name, name)) != current.get(mapping.get(name, name)) for name in managed):
            raise ManagementError("统一部署管理中的插件不能通过高级配置覆盖，请使用更新方案", 409)

    @staticmethod
    def _published_assignment(state, profile_id):
        releases = [record for record in state["requests"].values()
                    if record["profile_id"] == profile_id and record["stage"] in {"ready", "completed"}
                    and (record["mode"] == "update" or record["changing"])]
        if not releases:
            return None
        published = max(releases, key=lambda record: record["created_at"])
        for name, identity in published["target_ids"].items():
            current = state["targets"].get(name, {})
            if current.get("target_id") != identity or current.get("profile_id") != profile_id:
                raise DeploymentError("该方案的已发布版本已被其他统一目标替代，请选择当前已发布方案", 409)
        return published

    def _snapshot(self, profile_id, body):
        if not isinstance(body, dict):
            raise DeploymentError("request body must be an object")
        profile = self.profiles.get(profile_id)
        mode = body.get("mode", "assign" if body.get("client_ids") else "update")
        if mode not in {"update", "assign"}:
            raise DeploymentError("mode must be update or assign")
        state = self._read()
        replaces = body.get("replaces_rollout_id")
        published = self._published_assignment(state, profile_id) if mode == "assign" and not replaces else None
        if published:
            frozen = published["snapshot"]
            plugin_configs = {name: copy.deepcopy(target["config"]) for name, target in frozen["targets"].items()}
            schedule = copy.deepcopy(frozen["schedule"])
            resource_summary = copy.deepcopy(published["resource_summary"])
        else:
            if body.get("revision") != profile["revision"]:
                raise DeploymentError("部署方案已改变，请重新预览", 409, current_revision=profile["revision"])
            prepared = self.profiles.compile(profile)
            defaults = self.fleet.defaults(masked=False)["config_patch"].get("plugins", {})
            plugin_configs = {name: merge_config(defaults.get(name, {}), fields)
                              for name, fields in prepared["config_patch"]["plugins"].items()}
            schedule = prepared["config_patch"]["client"]
            resource_summary = [{"plugin": name, "resources": [
                {"label": role, "name": self.deployments.artifacts.get(selection["artifact_id"])["name"],
                 "version": self.deployments.artifacts.get(selection["artifact_id"])["version"]}
                for role, selection in settings["resources"].items()]} for name, settings in profile["plugins"].items()]
        if replaces:
            previous = self._record(state, replaces)
            if (body.get("replaces_revision") != previous["revision"]
                    or previous["stage"] not in {"switching", "deploying"}
                    or set(previous["plugins"]) != set(plugin_configs)):
                raise DeploymentError("只可用相同插件范围修复尚未可运行的切换请求，请刷新后重试", 409)
        if self._pending(state, plugin_configs, ignore=replaces):
            raise DeploymentError("同一插件已有待更新请求，请先完成或取消该请求", 409)
        inventory = self.fleet.agents()
        if not inventory["scheduler_online"]:
            raise ManagementError("调度器离线，无法固定旧批次边界，请恢复服务后提交", 503)
        rows = {row["client_id"]: row for row in inventory["agents"]}
        assignments = {client: set() for client in (self.fleet._client_ids(body.get("client_ids")) if mode == "assign" else [])}
        for client in assignments:
            if client not in rows:
                raise DeploymentError(f"客户端 {client} 尚未注册", 409)
            assignments[client].update(plugin_configs)
        for client, row in ([] if published else rows.items()):
            desired = (row.get("deployment") or {}).get("desired") or {}
            enabled = {name for name, fields in desired.get("config_patch", {}).get("plugins", {}).items()
                       if fields.get("enabled") is True}
            enabled.update(row.get("plugin_keys", []))
            enabled.update(worker.get("plugin_type") for worker in row.get("workers", []))
            selected = enabled & set(plugin_configs)
            if selected:
                assignments.setdefault(client, set()).update(selected)
        if replaces:
            for client, item in previous["clients"].items():
                assignments.setdefault(client, set()).update(item["plugins"])
                rows.setdefault(client, {"client_id": client, "online": False})
        if not assignments:
            raise DeploymentError("没有启用该插件的机器，请先选择新机器分配方案", 409)
        scheduler = self.management.get_config("scheduler")
        native_status = self.management.get_status()
        config_keys = native_status.get("plugin_config_keys", {})
        configured = (scheduler.get("config") or {}).get("plugins", {})
        if not scheduler.get("available") or scheduler.get("restart_required"):
            raise ManagementError("请先使服务端已保存配置生效，再提交部署更新", 409)
        missing = [name for name in plugin_configs if not configured.get(config_keys.get(name, name), {}).get("enabled")]
        if missing:
            raise ManagementError("请先安装并启用服务端插件：" + "、".join(missing), 409)
        targets = copy.deepcopy(frozen["targets"]) if published else {}
        for name, config in ([] if published else plugin_configs.items()):
            components = [item for item in prepared["components"] if item["slot"].startswith("profile." + name + ".")]
            identity = digest({"plugin": name, "config": config, "components": components,
                               "scheduler": prepared["scheduler_plugins"].get(name, {})})
            config["deployment_target_id"] = identity
            targets[name] = {"target_id": identity, "config": config, "components": components,
                             "scheduler": prepared["scheduler_plugins"].get(name, {})}
            current = state["targets"].get(name)
            if mode == "assign" and current and current.get("profile_id") != profile_id and not replaces:
                raise DeploymentError(f"{name} 已由其他统一方案发布，请选择当前已发布方案", 409)
            if mode == "assign" and current and current["target_id"] != identity and not replaces:
                raise DeploymentError(f"{name} 已有统一版本，请先更新该插件全局目标再分配机器", 409)
        plans = []
        for client, names in sorted(assignments.items()):
            plans.append(self._plan(client, targets, names, schedule))
        waiting = []
        seen = set()
        for row in native_status.get("unfinished_runs", []) + native_status.get("active_runs", []) + native_status.get("queued_runs", []):
            if not published and row.get("plugin_type") in targets and row.get("run_id") not in seen:
                seen.add(row["run_id"])
                waiting.append({"run_id": row["run_id"], "plugin_type": row["plugin_type"], "status": row.get("status", "running")})
        snapshot = {"profile_id": profile_id,
                    "profile_revision": published["profile_revision"] if published else profile["revision"],
                    "profile_name": published["profile_name"] if published else profile["name"],
                    "published_target": bool(published),
                    "mode": mode, "targets": targets, "schedule": schedule,
                    "assignments": {client: sorted(names) for client, names in assignments.items()},
                    "plans": plans, "scheduler_revision": scheduler.get("revision"),
                    "replaces_rollout_id": replaces, "replaces_revision": body.get("replaces_revision")}
        snapshot["resource_summary"] = resource_summary
        snapshot["plan_fingerprint"] = digest(snapshot)
        warnings = []
        unsupported = []
        for client in assignments:
            if rows[client].get("online") and CAPABILITY not in rows[client].get("capabilities", []):
                unsupported.append(client)
                warnings.append(f"{client} 的基础客户端需要升级，才支持统一版本准入")
        preview = {key: snapshot[key] for key in ("profile_id", "profile_revision", "profile_name", "mode", "plan_fingerprint", "scheduler_revision", "resource_summary", "published_target")}
        preview.update(plugins=sorted(targets), target_ids={name: item["target_id"] for name, item in targets.items()},
            clients=[{**_masked(plan), "online": bool(rows[plan["client_id"]].get("online")),
                      "busy": bool(rows[plan["client_id"]].get("busy")),
                      "state": rows[plan["client_id"]].get("state", "offline")} for plan in plans],
            waiting_runs=waiting, warnings=warnings, unsupported_clients=unsupported,
            publish_blocked=bool(unsupported))
        return snapshot, preview

    def _plan(self, client, targets, names, schedule):
        desired = self.deployments.get(client)["desired"]
        patch = {"plugins": {name: targets[name]["config"] for name in names}}
        if not desired:
            patch["client"] = copy.deepcopy(schedule)
        components = [item for name in names for item in targets[name]["components"]]
        return self.fleet._plan([client], components, patch, defaults_override={}, replace_plugins=True)[0]

    def preview(self, profile_id, body):
        with self._lock, self.profiles._lock:
            return self._snapshot(profile_id, body)[1]

    def publish(self, profile_id, body):
        if not isinstance(body, dict):
            raise DeploymentError("request body must be an object")
        request_id = DeploymentStore._identifier(body.get("request_id"), "request_id")
        request_hash = digest({"profile_id": profile_id, **body})
        with self._lock, self.profiles._lock:
            state = self._read()
            existing = state["requests"].get(request_id)
            if existing:
                if existing["request_hash"] != request_hash:
                    raise DeploymentError("request_id 已用于另一个部署请求", 409)
                return self._public(existing)
            snapshot, preview = self._snapshot(profile_id, body)
            if preview["publish_blocked"]:
                raise DeploymentError("请先升级这些在线机器的基础客户端：" + "、".join(preview["unsupported_clients"]), 409)
            if body.get("plan_fingerprint") != snapshot["plan_fingerprint"]:
                raise DeploymentError("方案、模板、目标机器或配置已改变，请重新预览", 409)
            changing = [name for name, target in snapshot["targets"].items()
                        if snapshot.get("replaces_rollout_id") or state["targets"].get(name, {}).get("target_id") != target["target_id"]]
            record = {"rollout_id": request_id, "request_hash": request_hash,
                **{key: snapshot[key] for key in ("profile_id", "profile_revision", "profile_name", "mode")},
                "plugins": sorted(snapshot["targets"]), "target_ids": preview["target_ids"],
                "resource_summary": snapshot["resource_summary"],
                "published_target": snapshot["published_target"],
                "snapshot": snapshot, "changing": changing, "stage": "pending" if changing else "deploying",
                "replaces_rollout_id": snapshot.get("replaces_rollout_id"),
                "native": {}, "waiting_runs": preview["waiting_runs"], "created_at": now(),
                "operator_id": DeploymentStore._optional_identifier(body.get("operator_id"), "operator_id"),
                "reason": DeploymentStore._optional_text(body.get("reason"), "reason", 4096),
                "clients": {client: {"client_id": client, "plugins": names, "status": "pending", "attempt": 0}
                            for client, names in snapshot["assignments"].items()}}
            self._save(state, record)
            if record.get("replaces_rollout_id"):
                previous = state["requests"][record["replaces_rollout_id"]]
                previous["replacement_pending"] = record["rollout_id"]
                self._save(state, previous)
            # Begin is part of acceptance, so the native queue boundary is fixed
            # before returning. An unknown response retains the durable intent.
            try:
                self._begin(record)
            except (ManagementError, DeploymentError) as error:
                record["error"] = str(error)
            self._save(state, record)
            return self._public(record)

    def _begin(self, record):
        if record["stage"] != "pending":
            return
        for name in record["changing"]:
            target = record["snapshot"]["targets"][name]
            result = self.management.plugin_rollout("begin_plugin_rollout", name, record["rollout_id"],
                config_revision=record["snapshot"]["scheduler_revision"], target_id=target["target_id"], target_patch=target["scheduler"],
                **({"replaces_rollout_id": record["replaces_rollout_id"]} if record.get("replaces_rollout_id") else {}))
            record["native"][name] = result
        record["stage"] = "switching" if record.get("replaces_rollout_id") else "waiting_runs"

    def tick(self):
        with self._lock:
            state = self._read()
            for record in sorted(state["requests"].values(), key=lambda row: row["created_at"]):
                if self._stop.is_set():
                    break
                if record["stage"] in TERMINAL:
                    continue
                if record.get("replacement_pending"):
                    continue
                try:
                    record.pop("error", None)
                    self._advance(state, record)
                except (ManagementError, DeploymentError, OSError) as error:
                    record["error"] = str(error)
                self._save(state, record)

    def _advance(self, state, record):
        if record.get("cancel_requested"):
            for name in record["changing"]:
                try:
                    self.management.plugin_rollout("get_plugin_rollout", name, record["rollout_id"])
                    self.management.plugin_rollout("cancel_plugin_rollout", name, record["rollout_id"])
                except ManagementError as error:
                    if error.status != 404:
                        raise
            record["stage"] = "cancelled"
            return
        self._begin(record)
        if record.get("replaces_rollout_id") and record["stage"] != "pending":
            previous = state["requests"][record["replaces_rollout_id"]]
            previous["stage"] = "superseded"
            self._save(state, previous)
        if record["stage"] == "waiting_runs":
            blockers = []
            ready = True
            for name in record["changing"]:
                result = self.management.plugin_rollout("get_plugin_rollout", name, record["rollout_id"])
                record["native"][name] = result
                blockers.extend({**row, "plugin_type": name} for row in result.get("blockers", []))
                ready = ready and bool(result.get("ready_to_switch"))
            record["waiting_runs"] = blockers
            if not ready:
                return
            record["stage"] = "switching"
            self._save(state, record)
        if record["stage"] == "switching":
            for name in record["changing"]:
                native = self.management.plugin_rollout("get_plugin_rollout", name, record["rollout_id"])
                if native.get("phase") == "waiting":
                    self.management.plugin_rollout("activate_plugin_rollout", name, record["rollout_id"])
                target = record["snapshot"]["targets"][name]
                config = self.management.get_config("scheduler")
                config_key = self.management.get_status().get("plugin_config_keys", {}).get(name, name)
                current = config.get("config", {}).get("plugins", {}).get(config_key, {})
                if any(current.get(key) != value for key, value in target["scheduler"].items()):
                    self.management.apply_plugin_deployment(config["revision"], {name: target["scheduler"]}, rollout_id=record["rollout_id"])
                state["targets"][name] = {**target, "profile_id": record["profile_id"],
                    "profile_name": record["profile_name"], "rollout_id": record["rollout_id"], "ready": False}
                self._save(state, record)
            record["stage"] = "deploying"
            self._save(state, record)
        if record["stage"] in {"deploying", "ready"}:
            inventory = self.fleet.agents()
            if not inventory["scheduler_online"]:
                raise ManagementError("调度器离线，恢复后继续更新", 503)
            rows = {row["client_id"]: row for row in inventory["agents"]}
            for item in record["clients"].values():
                try:
                    self._machine(state, record, item, rows.get(item["client_id"], {}))
                except (ManagementError, DeploymentError, OSError) as error:
                    item["error"] = str(error)
                    # Storage or transport failures retain their ownership token
                    # for recovery; do not call them a normal idle wait.
                    item["status"] = "failed"
                    item["recoverable_error"] = True
                self._save(state, record)
            if record["stage"] == "deploying":
                if any(not any(item["status"] == "ready" and item.get("online") and name in item["plugins"]
                               for item in record["clients"].values()) for name in record["changing"]):
                    return
                for name in record["changing"]:
                    native = self.management.plugin_rollout("get_plugin_rollout", name, record["rollout_id"])
                    if native.get("phase") != "completed":
                        self.management.plugin_rollout("finish_plugin_rollout", name, record["rollout_id"])
                    state["targets"][name]["ready"] = True
                record["stage"] = "ready"
            if all(item["status"] == "ready" for item in record["clients"].values()):
                record["stage"] = "completed"

    def _machine(self, state, record, item, row):
        client = item["client_id"]
        item["online"] = bool(row.get("online"))
        # A newer plugin target supersedes the old request's catch-up. Never
        # resurrect its old snapshot when an offline machine returns.
        targets = copy.deepcopy(record["snapshot"]["targets"])
        for name in item["plugins"]:
            if name in state["targets"]:
                targets[name] = state["targets"][name]
        target_ids = {name: targets[name]["target_id"] for name in item["plugins"]}
        if item.get("target_ids") != target_ids:
            item.update(target_ids=target_ids, attempt=item["attempt"] + 1, status="pending")
            for key in ("token", "revision", "error"):
                item.pop(key, None)
        desired_state = self.deployments.get(client)
        desired = desired_state["desired"] or {}
        token = item.get("token")
        if token and desired.get("reservation_id") == token:
            item["revision"] = desired["revision"]
            if desired_state["state"] in {"current", "failed"}:
                self.fleet.get(client)  # Repair a lost completion/release reply.
                item["status"] = "ready" if desired_state["state"] == "current" else "failed"
                if item["status"] == "failed":
                    item["error"] = (desired_state.get("reported") or {}).get("error") or "客户端应用失败，可重试"
                else:
                    item.pop("error", None)
                return
            if row.get("update_window_open") is False and desired_state["state"] == "waiting_window":
                self._release_window(client, desired_state, row)
                item["status"] = "waiting_window"
                item.pop("error", None)
                return
            if item["online"]:
                self.fleet.get(client)  # Recommit durable desired before serving it.
            status = desired_state["state"]
            item["status"] = ("offline" if not item["online"] else "downloading" if status == "staged"
                              else status if status in {"waiting_window", "waiting_idle", "applying"} else "pending")
            return
        matching = all(desired.get("config_patch", {}).get("plugins", {}).get(name, {}).get("deployment_target_id") == identity
                       for name, identity in target_ids.items())
        if matching and desired_state["state"] == "current":
            item.update(status="ready", revision=desired.get("revision"))
            return
        if item["status"] == "failed" and not item.get("recoverable_error"):
            return
        if not item["online"]:
            item["status"] = "offline"
            return
        if CAPABILITY not in row.get("capabilities", []):
            item.update(status="waiting_idle", error="基础客户端需要升级以支持统一版本准入")
            return
        item.pop("error", None)
        item.pop("recoverable_error", None)
        if row.get("update_window_open") is False:
            item["status"] = "waiting_window"
            return
        if not row.get("configurable") and row.get("reservation_id") != token:
            item["status"] = "waiting_idle"
            return
        plan = self._plan(client, targets, item["plugins"], record["snapshot"]["schedule"])
        token = token or digest({"rollout": record["rollout_id"], "client": client, "attempt": item["attempt"]})
        item["token"] = token
        self._save(state, record)
        self.management.client_control("reserve_clients", client_ids=[client], reservation_id=token,
                                       require_update_window_open=True)
        try:
            published = self.deployments.put_batch([plan], token, operator_id=record.get("operator_id"), reason=record.get("reason"))
        except Exception:
            self.management.client_control("release_clients", client_ids=[client], reservation_id=token)
            raise
        item.update(revision=published[0]["desired"]["revision"], status="pending")
        self._save(state, record)
        self.management.client_control("commit_reservation", client_ids=[client], reservation_id=token)

    def operation(self, rollout_id, action, body):
        if not isinstance(body, dict):
            raise DeploymentError("request body must be an object")
        with self._lock:
            state = self._read()
            record = self._record(state, rollout_id)
            if body.get("revision") != record["revision"]:
                raise DeploymentError("部署请求已改变，请刷新后重试", 409, current_revision=record["revision"])
            if action == "cancel":
                if record["stage"] not in {"pending", "waiting_runs"} or record.get("replaces_rollout_id"):
                    raise DeploymentError("部署已开始切换，不能取消", 409)
                record["cancel_requested"] = True
                self._save(state, record)
                self._advance(state, record)
            elif action == "retry":
                if record["stage"] in TERMINAL:
                    raise DeploymentError("该更新请求已结束", 409)
                selected = body.get("client_ids", list(record["clients"]))
                if not isinstance(selected, list) or any(client not in record["clients"] for client in selected):
                    raise DeploymentError("重试机器必须属于此更新请求")
                for client in selected:
                    item = record["clients"][client]
                    if item["status"] == "failed":
                        if item.pop("recoverable_error", False):
                            item.update(status="pending")
                            item.pop("error", None)
                            continue
                        self.fleet.get(client)
                        item.update(status="pending", attempt=item["attempt"] + 1)
                        for key in ("token", "revision", "error"):
                            item.pop(key, None)
                record.pop("error", None)
            elif action in {"resume", "abandon"}:
                plugin = body.get("plugin_type")
                if record["stage"] != "waiting_runs" or plugin not in record["changing"]:
                    raise DeploymentError("只能处理当前更新边界内已停止的旧批次", 409)
                values = {"run_id": body.get("run_id")}
                if action == "abandon":
                    if not isinstance(body.get("reason"), str) or not body["reason"].strip():
                        raise DeploymentError("放弃批次必须填写原因")
                    values["reason"] = body["reason"].strip()
                self.management.plugin_rollout(action + "_rollout_run", plugin, rollout_id, **values)
            else:
                raise DeploymentError("未知部署操作", 404)
            self._save(state, record)
            return self._public(record)
