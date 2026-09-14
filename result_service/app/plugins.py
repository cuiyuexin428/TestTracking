from __future__ import annotations

import re
from typing import Any, Protocol


class QueryConnection(Protocol):
    def execute(self, query: str, params: tuple[Any, ...] = ()) -> Any: ...


class ResultPluginError(RuntimeError):
    """A plugin-owned result query could not be completed."""


class ResultPlugin(Protocol):
    name: str
    configured: bool

    def summary(self, connection: QueryConnection, run_id: str) -> dict[str, Any]: ...


class CoverageResultPlugin:
    name = "coverage"
    configured = True

    def summary(self, connection: QueryConnection, run_id: str) -> dict[str, Any]:
        try:
            relation = connection.execute(
                "SELECT to_regclass('coverage_run_summary') AS relation"
            ).fetchone()
            if not relation or not relation["relation"]:
                return {}
            row = connection.execute(
                "SELECT total_files, selected_files, cumulative_points, covered_lines, "
                "total_lines, percent, finished_at FROM coverage_run_summary WHERE run_id=%s",
                (run_id,),
            ).fetchone()
        except Exception as exc:
            raise ResultPluginError("coverage result query failed") from exc
        return dict(row) if row else {}


class StpCompareResultPlugin:
    name = "stpcompare"

    def __init__(self, table: str | None):
        self.table = table if table and _valid_identifier(table) else None
        self.configured = self.table is not None

    def summary(self, connection: QueryConnection, run_id: str) -> dict[str, Any]:
        if not self.table:
            return {}
        try:
            row = connection.execute(
                f"SELECT count(*) FILTER (WHERE o.status='success' AND r.info_txt IS NOT NULL "
                f"AND r.info_new_txt IS NOT NULL AND r.info_txt = r.info_new_txt) AS same, "
                f"count(*) FILTER (WHERE o.status='success' AND r.info_txt IS NOT NULL "
                f"AND r.info_new_txt IS NOT NULL AND r.info_txt <> r.info_new_txt) AS different, "
                f"count(*) FILTER (WHERE o.status IS DISTINCT FROM 'success' "
                f"OR r.file IS NULL OR r.info_txt IS NULL OR r.info_new_txt IS NULL) AS incomplete "
                f"FROM plugin_run_files m "
                f"LEFT JOIN plugin_file_outcomes o ON o.run_id=m.run_id "
                f"AND o.plugin_type=m.plugin_type AND o.file=m.file "
                f"LEFT JOIN {self.table} r ON r.run_id=m.run_id AND r.file=m.file "
                f"WHERE m.run_id=%s AND m.plugin_type='stpcompare'",
                (run_id,),
            ).fetchone()
        except Exception as exc:
            raise ResultPluginError("stpcompare result query failed") from exc
        return dict(row) if row else {}


def _valid_identifier(value: str) -> bool:
    return bool(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value))


def default_plugins(stp_result_table: str | None) -> dict[str, ResultPlugin]:
    return {"coverage": CoverageResultPlugin(),
            "stpcompare": StpCompareResultPlugin(stp_result_table)}
