from __future__ import annotations

import logging
from typing import Any
from contextlib import contextmanager

from .plugins import ResultPlugin, ResultPluginError

LOG = logging.getLogger("result_service.repository")

TASK_PROGRESS_STATUSES = ("reported", "acked", "dispatched", "reclaimed", "pending")


def result_readiness(run: dict[str, Any]) -> dict[str, Any]:
    """Describe durable file results independently of execution/settlement state."""
    expected = run.get("expected_files")
    expected = int(expected) if expected is not None else None
    outcomes = run.get("outcomes", {})
    success = int(run.get("success_files", outcomes.get("success", 0)) or 0)
    failed = int(run.get("failed_files", outcomes.get("failed", 0)) or 0)
    reported = success + failed
    status = "empty" if not reported else (
        "unknown" if not expected else ("complete" if reported >= expected else "partial")
    )
    return {"status": status, "can_analyze": reported > 0,
            "expected_files": expected, "reported_files": reported,
            "successful_files": success, "failed_files": failed,
            "missing_files": max(0, expected - reported) if expected is not None else None}


class RepositoryError(RuntimeError):
    pass


class NullRepository:
    """Repository used when the service is started without a database."""

    configured = False

    def ready(self) -> bool:
        return False

    def list_runs(self, plugin: str | None = None, limit: int = 50, offset: int = 0,
                  search: str = "") -> list[dict[str, Any]]:
        return []

    def get_run(self, run_id: str, plugin: str | None = None) -> dict[str, Any] | None:
        return None

    def analysis_runs(self, plugin: str | None = None, limit: int = 50, offset: int = 0,
                      search: str = "") -> list[dict[str, Any]]:
        return []

    def analysis_run(self, run_id: str, plugin: str, *, include_summary: bool = True) -> dict[str, Any] | None:
        return None

    def database_schema(self) -> list[dict[str, Any]]:
        return []


class PostgresRepository:
    configured = True

    def __init__(self, conninfo: str, plugins: dict[str, ResultPlugin]):
        self.conninfo = conninfo
        self.plugins = plugins

    @contextmanager
    def _connect(self):
        try:
            import psycopg
            from psycopg.rows import dict_row
        except ImportError as exc:
            raise RepositoryError("psycopg is required when RESULT_SERVICE_DB is configured") from exc
        try:
            with psycopg.connect(self.conninfo, row_factory=dict_row, connect_timeout=3) as connection:
                # Defense in depth: the configured role should have SELECT
                # privileges only, and every transaction is read-only as well.
                connection.read_only = True
                connection.execute("SET statement_timeout = '5s'")
                yield connection
        except psycopg.Error as exc:
            raise RepositoryError("database query failed; check service database configuration and logs") from exc

    def ready(self) -> bool:
        try:
            with self._connect() as connection:
                connection.execute("SELECT 1 FROM plugin_runs LIMIT 0")
                connection.execute("SELECT 1 FROM plugin_file_outcomes LIMIT 0")
            return True
        except Exception:
            return False

    def list_runs(self, plugin: str | None = None, limit: int = 50, offset: int = 0,
                  search: str = "") -> list[dict[str, Any]]:
        return self._list_runs(plugin, limit, offset, search, include_progress=True)

    def analysis_runs(self, plugin: str | None = None, limit: int = 50, offset: int = 0,
                      search: str = "") -> list[dict[str, Any]]:
        return self._list_runs(plugin, limit, offset, search, include_progress=False)

    def _list_runs(self, plugin: str | None, limit: int, offset: int, search: str,
                   *, include_progress: bool) -> list[dict[str, Any]]:
        limit = max(1, min(int(limit), 200))
        offset = max(0, int(offset))
        clauses: list[str] = []
        params: list[Any] = []
        if plugin:
            clauses.append("r.plugin_type=%s")
            params.append(plugin)
        if search.strip():
            # Treat run IDs as literal text, including SQL LIKE wildcard characters.
            clauses.append("strpos(lower(r.run_id), lower(%s)) > 0")
            params.append(search.strip())
        where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
        query = (
            "SELECT r.run_id, r.plugin_type, r.expected_files, "
            "COALESCE((to_jsonb(r)->>'planned_tasks')::int, 0) AS planned_tasks, "
            "r.status, r.program_version, "
            "r.dataset_version, r.created_at, r.finished_at, "
            "count(o.file) FILTER (WHERE o.status='success') AS success_files, "
            "count(o.file) FILTER (WHERE o.status='failed') AS failed_files "
            "FROM plugin_runs r LEFT JOIN plugin_file_outcomes o "
            "ON o.run_id=r.run_id AND o.plugin_type=r.plugin_type"
            + where + " GROUP BY r.run_id, r.plugin_type ORDER BY r.created_at DESC, r.run_id DESC LIMIT %s OFFSET %s"
        )
        params.extend((limit, offset))
        with self._connect() as connection:
            runs = [dict(row) for row in connection.execute(query, tuple(params)).fetchall()]
            progress_by_run: dict[str, dict[str, Any]] = {}
            if runs and include_progress:
                try:
                    # Scheduler history can be absent from an analysis-only database.
                    # A savepoint keeps durable results usable if those tables are absent.
                    with connection.transaction():
                        progress_by_run = self._execution_progress(connection, runs)
                except Exception as exc:
                    if getattr(exc, "sqlstate", None) != "42P01":
                        raise
                    LOG.debug("scheduler history is unavailable", exc_info=True)

            for run in runs:
                run["result_readiness"] = result_readiness(run)
                if not include_progress:
                    continue
                progress = progress_by_run.get(run["run_id"], {
                    "recorded_tasks": 0,
                    "task_status_counts": dict.fromkeys(TASK_PROGRESS_STATUSES, 0),
                })
                planned = max(0, int(run.get("planned_tasks") or 0))
                recorded = progress.pop("recorded_tasks")
                total = max(planned, recorded)
                if run.get("status") == "finished" and recorded == 0 and total > 0:
                    progress["task_status_counts"]["reported"] = total
                else:
                    progress["task_status_counts"]["pending"] = max(0, total - recorded)
                run.update(progress)
                run["total_tasks"] = total
            return runs

    @staticmethod
    def _execution_progress(connection, runs):
        progress_by_run: dict[str, dict[str, Any]] = {}
        run_ids = [run["run_id"] for run in runs]
        task_rows = connection.execute(
            "SELECT run_id, status, count(*) AS count FROM tasks "
            "WHERE run_id=ANY(%s) GROUP BY run_id, status",
            (run_ids,),
        ).fetchall()
        for task_row in task_rows:
            run_id = task_row["run_id"]
            status = task_row["status"] or "unknown"
            count = int(task_row["count"])
            progress = progress_by_run.setdefault(
                run_id,
                {
                    "recorded_tasks": 0,
                    "task_status_counts": dict.fromkeys(TASK_PROGRESS_STATUSES, 0),
                },
            )
            progress["recorded_tasks"] += count
            progress["task_status_counts"][status] = count

        scheduler_rows = connection.execute(
            "SELECT run_id, status, finished_at FROM runs WHERE run_id=ANY(%s)",
            (run_ids,),
        ).fetchall()
        scheduler_by_run = {row["run_id"]: row for row in scheduler_rows}
        for run in runs:
            scheduler_run = scheduler_by_run.get(run["run_id"])
            if scheduler_run:
                run["status"] = scheduler_run["status"]
                if scheduler_run.get("finished_at") is not None:
                    run["finished_at"] = scheduler_run["finished_at"]

        return progress_by_run

    def database_schema(self) -> list[dict[str, Any]]:
        with self._connect() as connection:
            rows = connection.execute(
                "SELECT table_schema, table_name, column_name, data_type "
                "FROM information_schema.columns "
                "WHERE table_schema NOT IN ('pg_catalog', 'information_schema') "
                "ORDER BY table_schema, table_name, ordinal_position"
            ).fetchall()
        tables: dict[str, dict[str, Any]] = {}
        for row in rows:
            name = f"{row['table_schema']}.{row['table_name']}"
            table = tables.setdefault(name, {"name": name, "columns": []})
            table["columns"].append({"name": row["column_name"], "type": row["data_type"]})
        return list(tables.values())

    def analysis_run(self, run_id: str, plugin: str, *, include_summary: bool = True) -> dict[str, Any] | None:
        return self._get_run(run_id, plugin, include_execution=False, include_summary=include_summary)

    def get_run(self, run_id: str, plugin: str | None = None) -> dict[str, Any] | None:
        return self._get_run(run_id, plugin, include_execution=True)

    def _get_run(self, run_id: str, plugin: str | None, *, include_execution: bool,
                 include_summary: bool = True) -> dict[str, Any] | None:
        status = "COALESCE(s.status,p.status)" if include_execution else "p.status"
        finished = "COALESCE(s.finished_at,p.finished_at)" if include_execution else "p.finished_at"
        join = " LEFT JOIN runs s ON s.run_id=p.run_id " if include_execution else " "
        query = (
            "SELECT p.run_id, p.plugin_type, p.expected_files, p.manifest_fingerprint, "
            f"{status} AS status, p.program_version, p.dataset_version, "
            f"p.created_at, {finished} AS finished_at "
            "FROM plugin_runs p" + join +
            "WHERE p.run_id=%s" + (" AND p.plugin_type=%s" if plugin else "")
        )
        params = (run_id, plugin) if plugin else (run_id,)
        with self._connect() as connection:
            row = connection.execute(query, params).fetchone()
            if not row:
                return None
            result = dict(row)
            outcome_rows = connection.execute(
                "SELECT status, count(*) AS count FROM plugin_file_outcomes "
                "WHERE run_id=%s AND plugin_type=%s GROUP BY status",
                (run_id, result["plugin_type"]),
            ).fetchall()
            result["outcomes"] = {item["status"]: item["count"] for item in outcome_rows}
            result["result_readiness"] = result_readiness(result)
            readiness = result["result_readiness"]
            result["result_completeness"] = {
                key: readiness[key] for key in (
                    "expected_files", "reported_files", "successful_files", "failed_files")
            }
            # Pending includes files without an outcome; failed is already reported.
            result["result_completeness"]["pending_files"] = readiness["missing_files"]
            adapter = self.plugins.get(result["plugin_type"])
            if adapter and include_summary:
                if not adapter.configured:
                    result["analysis"] = {}
                    result["analysis_status"] = "not_configured"
                else:
                    try:
                        with connection.transaction():
                            result["analysis"] = adapter.summary(connection, run_id)
                        result["analysis_status"] = "ready" if result["analysis"] else "pending"
                    except ResultPluginError:
                        LOG.warning("result analysis query failed for %s/%s",
                                    result["plugin_type"], run_id, exc_info=True)
                        result["analysis"] = {}
                        result["analysis_status"] = "error"
            return result


def create_repository(conninfo: str, plugins: dict[str, ResultPlugin]):
    return PostgresRepository(conninfo, plugins) if conninfo.strip() else NullRepository()
