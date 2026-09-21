"""In-memory scheduler control double for Python HTTP/coordinator contracts.

Native protocol and packaged integration tests exercise the real scheduler.
"""
import copy
import threading
from types import SimpleNamespace

from result_service.app.management import ManagementError


class FleetManagement:
    def __init__(self, template=None):
        self.config = SimpleNamespace(client_config_path=template)
        self.rows = {}
        self.workers = []
        self.reservations = {}
        self.online = True
        self.fail_commit = False
        self.calls = []
        self._lock = threading.RLock()

    def register(self, client, **values):
        self.rows[client] = {"client_id": client, "online": True, "busy": False,
                             "state": "unconfigured", **values}

    def get_config(self, name):
        return {"available": True, "revision": "scheduler-1", "config": {"plugins": {}}}

    def get_status(self):
        rows = copy.deepcopy(list(self.rows.values()))
        for row in rows:
            row["reservation_id"] = self.reservations.get(row["client_id"], {}).get("token", "")
        return {"online": self.online, "agents": rows if self.online else [], "clients": self.workers}

    def client_control(self, action, **body):
        with self._lock:
            self.calls.append((action, copy.deepcopy(body)))
            if not self.online:
                raise ManagementError("scheduler offline", 503)
            if action == "agent_heartbeat":
                self.register(body["client_id"], **{k: v for k, v in body.items() if k != "client_id"})
                return {"ok": True}
            token = body["reservation_id"]
            ids = body["client_ids"]
            unavailable = []
            for client in ids:
                reservation = self.reservations.get(client)
                row = self.rows.get(client, {})
                reason = None
                if reservation and reservation["token"] != token:
                    reason = "reserved"
                elif action == "reserve_clients" and not reservation:
                    if not row.get("online"): reason = "offline"
                    elif row.get("busy"): reason = "busy"
                    elif row.get("state") == "updating": reason = row["state"]
                elif action == "commit_reservation" and not reservation:
                    reason = "reservation_missing"
                if reason: unavailable.append({"client_id": client, "reason": reason})
            if unavailable:
                raise ManagementError("unavailable clients", 409, details={"unavailable": unavailable})
            if action == "commit_reservation" and self.fail_commit:
                raise ManagementError("lost commit response", 503)
            for client in ids:
                if action == "release_clients": self.reservations.pop(client, None)
                elif action == "reserve_clients":
                    self.reservations.setdefault(client, {"token": token, "committed": False})
                else: self.reservations[client]["committed"] = True
            return {"ok": True}
