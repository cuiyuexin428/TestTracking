"""Physical client discovery and idle-only, revision-checked batch deployment."""
from __future__ import annotations

import copy
import hashlib
import json
import logging
import threading
import uuid
from pathlib import Path
from typing import Any

from .deployments import DeploymentError, DeploymentStore, _ARTIFACT_REFERENCE
from .management import ManagementError, _masked

LOG = logging.getLogger("result_service.fleet")


def merge_config(base: Any, patch: Any) -> Any:
    if isinstance(base, dict) and isinstance(patch, dict):
        result = copy.deepcopy(base)
        for key, value in patch.items():
            result[key] = merge_config(result.get(key), value)
        return result
    return copy.deepcopy(patch)


class ClientFleet:
    def __init__(self, deployments, management):
        self.deployments = deployments
        self.management = management
        self._lock = threading.RLock()
        self._last_agents: dict[str, dict] = {}

    def heartbeat(self, client_id: Any, body: Any) -> dict:
        client = DeploymentStore._client_id(client_id)
        if not isinstance(body, dict):
            raise DeploymentError("heartbeat must be an object")
        allowed = {"instance_id", "hostname", "state", "plugin_keys", "agent_version",
                   "active_revision", "update_window_open", "capabilities"}
        return self.management.client_control("agent_heartbeat", client_id=client,
                                              **{key: value for key, value in body.items() if key in allowed})

    def agents(self) -> dict:
        status = self.management.get_status()
        with self._lock:
            current = {row["client_id"]: copy.deepcopy(row) for row in status.get("agents", [])}
            if status.get("online"):
                self._last_agents = copy.deepcopy(current)
            else:
                current = copy.deepcopy(self._last_agents)
                for row in current.values():
                    row.update(online=False)
            workers = status.get("clients", [])
            # Legacy workers remain visible but cannot be centrally configured
            # until their independently managed Agent is online.
            for worker in workers:
                physical = worker.get("physical_client_id") or worker["worker_id"]
                current.setdefault(physical, {"client_id": physical, "hostname": physical,
                                              "online": False, "state": "unmanaged"})
            for deployment in self.deployments.list():
                current.setdefault(deployment["client_id"], {"client_id": deployment["client_id"],
                                    "hostname": deployment["client_id"], "online": False, "state": "offline"})
                current[deployment["client_id"]]["deployment"] = deployment
            for physical, row in current.items():
                row["workers"] = [worker for worker in workers
                                  if (worker.get("physical_client_id") or worker["worker_id"]) == physical]
                row["busy"] = bool(row.get("busy") or any(worker.get("status") == "busy" or worker.get("tasks")
                                                          for worker in row["workers"]))
                row["configurable"] = bool(status.get("online") and row.get("online") and not row["busy"]
                                            and not row.get("reservation_id")
                                            and row.get("state") in {"ready", "unconfigured", "starting", "error"})
            return {"agents": list(sorted(current.values(), key=lambda item: item["client_id"])),
                    "scheduler_online": bool(status.get("online"))}

    def defaults(self, *, masked: bool = True) -> dict:
        """Server-owned plugin defaults; never copy a machine's identity/paths."""
        path = self.management.config.client_config_path
        plugins = {}
        if path and Path(path).is_file():
            try:
                raw = json.loads(Path(path).read_text(encoding="utf-8-sig"))
                plugins = copy.deepcopy(raw.get("plugins", {}))
            except (OSError, ValueError, TypeError) as exc:
                raise DeploymentError("客户端基础模板无法读取", 500) from exc
        if not isinstance(plugins, dict) or not all(isinstance(value, dict) for value in plugins.values()):
            raise DeploymentError("客户端基础模板 plugins 必须是对象", 500)
        for plugin in plugins.values():
            plugin["enabled"] = False
        result = {"config_patch": {"plugins": plugins}, "components": []}
        return _masked(result) if masked else result

    @staticmethod
    def _client_ids(value: Any) -> list[str]:
        if not isinstance(value, list) or not 1 <= len(value) <= 128:
            raise DeploymentError("请选择 1 到 128 台客户端")
        clients = [DeploymentStore._client_id(item) for item in value]
        if len(set(clients)) != len(clients):
            raise DeploymentError("客户端列表不能重复")
        return clients

    def _check_available(self, clients: list[str]) -> None:
        inventory = self.agents()
        if not inventory["scheduler_online"]:
            raise ManagementError("调度器离线，暂时不能发布客户端配置", 503)
        rows = {row["client_id"]: row for row in inventory["agents"]}
        unavailable = []
        for client in clients:
            row = rows.get(client, {})
            if not row.get("configurable"):
                reason = ("客户端管理进程未上线" if not row.get("online") else
                          "客户端正在执行任务或提交结果" if row.get("busy") else
                          "客户端正在更新")
                unavailable.append({"client_id": client, "reason": reason})
        if unavailable:
            raise ManagementError("部分客户端当前不可配置，请重新选择空闲客户端", 409,
                                  details={"unavailable": unavailable})

    def _plan(self, clients: list[str], components: Any, patch: Any, *,
              defaults_override: dict | None = None, replace_plugins: bool = False) -> list[dict]:
        component_patch = self.deployments._plan_components(components)
        if not isinstance(patch, dict):
            raise DeploymentError("config_patch must be an object")
        defaults = (self.defaults(masked=False)["config_patch"]["plugins"]
                    if defaults_override is None else defaults_override)
        plans = []
        for client in clients:
            state = self.deployments.get(client)
            desired = state["desired"] or {}
            original = copy.deepcopy(desired.get("config_patch", {}))
            if replace_plugins:
                for name in patch.get("plugins", {}):
                    original.get("plugins", {}).pop(name, None)
            incoming = copy.deepcopy(patch)
            slots = {item["slot"]: {"slot": item["slot"], "artifact_id": item["artifact_id"]}
                     for item in desired.get("components", [])}
            # Reserve resources still referenced by fields this operation does
            # not edit. Batch editing starts from a blank form, not client A.
            retained = copy.deepcopy(original)
            def remove_changed(old, changes):
                if not isinstance(old, dict) or not isinstance(changes, dict):
                    return
                for key, value in changes.items():
                    if isinstance(value, dict):
                        remove_changed(old.get(key), value)
                    else:
                        old.pop(key, None)
            remove_changed(retained, incoming)
            def references(value):
                found = set()
                if isinstance(value, dict):
                    for item in value.values(): found.update(references(item))
                elif isinstance(value, list):
                    for item in value: found.update(references(item))
                elif isinstance(value, str):
                    match = _ARTIFACT_REFERENCE.fullmatch(value)
                    if match: found.add(match[1])
                return found
            kept = references(retained)
            def replace_reference(value, old, new):
                if isinstance(value, dict):
                    return {key: replace_reference(item, old, new) for key, item in value.items()}
                if isinstance(value, list): return [replace_reference(item, old, new) for item in value]
                if isinstance(value, str) and value.startswith("{artifact:" + old + "}"):
                    return "{artifact:" + new + "}" + value[len(old) + 11:]
                return value
            for component in component_patch:
                slot = component["slot"]
                if slot in kept and slot in slots and slots[slot]["artifact_id"] != component["artifact_id"]:
                    if slot not in references(incoming):
                        raise DeploymentError(f"{client} 的 {slot} 仍由未修改的配置引用，请明确选择要更新的插件")
                    candidate = slot + "." + component["artifact_id"][:8]
                    if len(candidate) > 128 or (candidate in slots and slots[candidate]["artifact_id"] != component["artifact_id"]):
                        raise DeploymentError(f"{client} 的共享制品槽位冲突，请分配独立槽位")
                    incoming = replace_reference(incoming, slot, candidate)
                    slot = candidate
                slots[slot] = {"slot": slot, "artifact_id": component["artifact_id"]}
            merged = merge_config(original, incoming)
            plugins = merged.get("plugins", {})
            if not isinstance(plugins, dict):
                raise DeploymentError("config_patch.plugins must be an object")
            for name, plugin in plugins.items():
                if not isinstance(plugin, dict): raise DeploymentError("plugin configuration must be an object")
                if plugin.get("enabled") is True:
                    previous_plugin = original.get("plugins", {}).get(name, {})
                    if not previous_plugin.get("enabled"):
                        # A blank Agent has none of the server template's local
                        # binaries or input files. Require explicit resources.
                        paths = [("客户端插件包", plugin.get("dll"))]
                        if name in {"coverage", "stpcompare"}:
                            paths.append(("STP 数据包", plugin.get("stp_root")))
                            if name == "coverage":
                                paths.extend([("执行程序", plugin.get("target_exe")),
                                              ("覆盖率工具", plugin.get("opencppcoverage"))])
                            else:
                                programs = plugin.get("programs", {})
                                if not isinstance(programs, dict):
                                    raise DeploymentError("stpcompare.programs must be an object")
                                for role in ("baseline", "candidate"):
                                    program = programs.get(role, {})
                                    paths.append((role + " 执行程序", program.get("executable") if isinstance(program, dict) else None))
                        for label, path in paths:
                            if not isinstance(path, str) or not _ARTIFACT_REFERENCE.fullmatch(path):
                                raise DeploymentError(f"{client}: 首次启用 {name} 需要选择{label}，不能使用服务器模板的本机路径")
                    plugins[name] = merge_config(defaults.get(name, {}), plugin)
                    if not plugins[name].get("dll"):
                        raise DeploymentError(f"{client}: 请为 {name} 选择客户端插件包")
            value = self.deployments.prepare(list(slots.values()), merged)
            plans.append({"client_id": client, "revision": desired.get("revision"), **value})
        return plans

    def preview(self, body: Any) -> dict:
        if not isinstance(body, dict): raise DeploymentError("request body must be an object")
        clients = self._client_ids(body.get("client_ids"))
        self._check_available(clients)
        with self._lock:
            plans = self._plan(clients, body.get("components", []), body.get("config_patch", {}))
        bindings = {}
        conflicts = set()
        for plan in plans:
            for binding in plan["program_bindings"]:
                key = (binding["plugin"], binding["role"])
                if key in bindings and bindings[key]["version"] != binding["version"]: conflicts.add(key)
                bindings[key] = binding
        expected = {}
        for plan in plans:
            for plugin, values in plan["scheduler_plugins"].items():
                if not any(key[0] == plugin for key in conflicts): expected[plugin] = values
        scheduler = self.management.get_config("scheduler")
        warnings = ["仅修改本次指定的配置；每台机器未指定的插件、程序、数据及运行安排保持原值。"]
        warnings.append("发布后客户端停止领取新任务；若当前不在更新时间窗内，将保持预占并等待窗口打开后更新。")
        if conflicts: warnings.append("不同客户端的程序内容身份不同，请分批核对服务端程序匹配。")
        return {"clients": _masked(plans), "plan_fingerprint": self._plan_fingerprint(plans),
                "program_bindings": [binding for key, binding in bindings.items() if key not in conflicts],
                "scheduler_plugins": expected, "scheduler_revision": scheduler.get("revision"),
                "scheduler_restart_required": bool(expected), "warnings": warnings}

    @staticmethod
    def _plan_fingerprint(plans: list[dict]) -> str:
        # Include the full unmasked effective plan. Client revisions alone do
        # not detect template defaults changed between preview and publish.
        value = [{key: plan[key] for key in ("client_id", "revision", "components", "config_patch")}
                 for plan in sorted(plans, key=lambda item: item["client_id"])]
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                             ensure_ascii=False, allow_nan=False).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()

    def _publish(self, plans: list[dict], body: dict) -> dict:
        clients = [plan["client_id"] for plan in plans]
        token = uuid.uuid4().hex
        self.management.client_control("reserve_clients", client_ids=clients, reservation_id=token)
        try:
            published = self.deployments.put_batch(plans, token, operator_id=body.get("operator_id"), reason=body.get("reason"))
        except Exception:
            try: self.management.client_control("release_clients", client_ids=clients, reservation_id=token)
            except ManagementError: LOG.exception("Could not release unpublished client reservation")
            raise
        # A lost response or service restart after publication is repaired before
        # any Agent receives this desired revision. Never release a published
        # reservation just because the commit acknowledgement was lost.
        self.management.client_control("commit_reservation", client_ids=clients, reservation_id=token)
        return {"batch_id": token, "clients": published}

    def publish(self, body: Any) -> dict:
        if not isinstance(body, dict) or not isinstance(body.get("targets"), list):
            raise DeploymentError("targets must contain the previewed clients and revisions")
        if not all(isinstance(item, dict) and "revision" in item for item in body["targets"]):
            raise DeploymentError("each target requires client_id and revision")
        clients = self._client_ids([item.get("client_id") for item in body["targets"]])
        with self._lock:
            if getattr(self, "rollout_guard", None):
                self.rollout_guard(body)
            plans = self._plan(clients, body.get("components", []), body.get("config_patch", {}))
            for target, plan in zip(body["targets"], plans):
                expected = self.deployments._optional_revision(target["revision"], "revision")
                if expected != plan["revision"]:
                    raise DeploymentError(f"{plan['client_id']} 的部署已改变，请重新预览", 409,
                                          current_revision=plan["revision"])
            if body.get("plan_fingerprint") != self._plan_fingerprint(plans):
                raise DeploymentError("部署计划或客户端模板已改变，请重新预览后发布", 409)
            return self._publish(plans, body)

    def publish_single(self, client: str, body: Any) -> dict:
        client = DeploymentStore._client_id(client)
        if not isinstance(body, dict) or "revision" not in body:
            raise DeploymentError("revision is required")
        with self._lock:
            if getattr(self, "rollout_guard", None):
                self.rollout_guard(body, client=client)
            current = self.deployments.get(client)
            expected = self.deployments._optional_revision(body["revision"], "revision")
            revision = (current["desired"] or {}).get("revision")
            if expected != revision:
                raise DeploymentError("desired deployment changed; reload it before saving", 409,
                                      current_revision=revision)
            if body.get("action", "deploy") == "rollback":
                if "components" in body or "config_patch" in body:
                    raise DeploymentError("rollback uses the saved components and config_patch")
                target = self.deployments.historical(client, body.get("target_revision"))
                value = self.deployments.prepare(target["components"], target["config_patch"])
                plan = {"client_id": client, "revision": body["revision"], **value,
                        "action": "rollback", "rollback_of": target["revision"]}
            elif body.get("action", "deploy") == "deploy":
                value = self.deployments.prepare(body.get("components"), body.get("config_patch", {}))
                plan = {"client_id": client, "revision": body["revision"], **value}
            else: raise DeploymentError("action must be deploy or rollback")
            return self._publish([plan], body)["clients"][0]

    def get(self, client: str) -> dict:
        with self._lock:
            value = self.deployments.get(client)
            desired = value["desired"] or {}
            token = desired.get("reservation_id")
            if token and value["state"] not in {"current", "failed"}:
                managed = (self.rollout_delivery_guard(client, value)
                           if getattr(self, "rollout_delivery_guard", None) else False)
                self.management.client_control("reserve_clients", client_ids=[client], reservation_id=token,
                    **({"require_update_window_open": True} if managed else {}))
                self.management.client_control("commit_reservation", client_ids=[client], reservation_id=token)
            elif token:
                try:
                    self.management.client_control("release_clients", client_ids=[client], reservation_id=token)
                except ManagementError:
                    # Finished deployment details remain readable while the
                    # scheduler is offline. A subsequent poll retries release.
                    LOG.warning("Could not release completed reservation for %s; will retry", client)
            return value

    def report(self, client: str, body: Any) -> dict:
        with self._lock:
            value, _ = self.deployments.put_reported(client, body)
            token = value["desired"].get("reservation_id")
            if token and value["state"] in {"current", "failed"}:
                self.management.client_control("release_clients", client_ids=[client], reservation_id=token)
            return value
