from __future__ import annotations

from typing import Any
from contextlib import contextmanager

from .plugins import ResultPlugin


class RepositoryError(RuntimeError):
    pass


class NullRepository:
    """Repository used when the service is started without a database."""

    configured = False

    def ready(self) -> bool:
        return True

    def list_runs(self, plugin: str | None = None, limit: int = 50) -> list[dict[str, Any]]:
        return []

    def get_run(self, run_id: str, plugin: str | None = None) -> dict[str, Any] | None:
        return None


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
                connection.execute("SET default_transaction_read_only = on")
                connection.execute("SET statement_timeout = '5s'")
                yield connection
        except psycopg.Error as exc:
            raise RepositoryError("database query failed; check service database configuration and logs") from exc

    def ready(self) -> bool:
        try:
            with self._connect() as connection:
                connection.execute("SELECT 1").fetchone()
            return True
        except Exception:
            return False

    def list_runs(self, plugin: str | None = None, limit: int = 50) -> list[dict[str, Any]]:
        limit = max(1, min(int(limit), 200))
        clauses: list[str] = []
        params: list[Any] = []
        if plugin:
            clauses.append("r.plugin_type=%s")
            params.append(plugin)
        where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
        query = (
            "SELECT r.run_id, r.plugin_type, r.expected_files, r.status, r.program_version, "
            "r.dataset_version, r.created_at, r.finished_at, "
            "count(o.file) FILTER (WHERE o.status='success') AS success_files, "
            "count(o.file) FILTER (WHERE o.status='failed') AS failed_files "
            "FROM plugin_runs r LEFT JOIN plugin_file_outcomes o "
            "ON o.run_id=r.run_id AND o.plugin_type=r.plugin_type"
            + where + " GROUP BY r.run_id, r.plugin_type ORDER BY r.created_at DESC LIMIT %s"
        )
        params.append(limit)
        with self._connect() as connection:
            return [dict(row) for row in connection.execute(query, tuple(params)).fetchall()]

    def get_run(self, run_id: str, plugin: str | None = None) -> dict[str, Any] | None:
        query = (
            "SELECT run_id, plugin_type, expected_files, manifest_fingerprint, status, "
            "program_version, dataset_version, created_at, finished_at FROM plugin_runs "
            "WHERE run_id=%s" + (" AND plugin_type=%s" if plugin else "")
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
            result["result_completeness"] = {
                "expected_files": result.get("expected_files", 0),
                "reported_files": sum(result["outcomes"].values()),
                "successful_files": result["outcomes"].get("success", 0),
                "failed_files": result["outcomes"].get("failed", 0),
                "pending_files": result["outcomes"].get("pending", 0),
            }
            adapter = self.plugins.get(result["plugin_type"])
            if adapter:
                result["analysis"] = adapter.summary(connection, run_id)
            return result


def create_repository(conninfo: str, plugins: dict[str, ResultPlugin]):
    return PostgresRepository(conninfo, plugins) if conninfo.strip() else NullRepository()
