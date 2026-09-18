from __future__ import annotations

from typing import Any, Mapping


class CoverageResultAdapter:
    configured = True

    def summary(self, connection, run_id: str) -> dict[str, Any]:
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
        return dict(row) if row else {}


def create_result_adapter(options: Mapping[str, Any]) -> CoverageResultAdapter:
    return CoverageResultAdapter()
