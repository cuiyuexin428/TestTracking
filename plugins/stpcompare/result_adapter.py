from __future__ import annotations

import re
from typing import Any, Mapping


class StpCompareResultAdapter:
    def __init__(self, table: Any):
        self.table = table if isinstance(table, str) and re.fullmatch(
            r"[A-Za-z_][A-Za-z0-9_]*", table
        ) else None
        self.configured = self.table is not None

    def summary(self, connection, run_id: str) -> dict[str, Any]:
        if not self.table:
            return {}
        row = connection.execute(
            f"WITH raw AS (SELECT run_id,file,count(*) FILTER (WHERE content IS NOT NULL "
            f"AND content_sha256 IS NOT NULL AND program_role IN ('baseline','candidate') "
            f"AND export_stage IN ('first','second')) AS output_count, "
            f"coalesce(sum(content_size),0) AS raw_bytes FROM {self.table} GROUP BY run_id,file) "
            f"SELECT count(*) FILTER (WHERE o.status='success' AND r.output_count=4) AS complete, "
            f"count(*) FILTER (WHERE o.status IS DISTINCT FROM 'success' "
            f"OR coalesce(r.output_count,0)<>4) AS incomplete, "
            f"coalesce(sum(r.output_count),0) AS raw_outputs, "
            f"coalesce(sum(r.raw_bytes),0) AS raw_bytes "
            f"FROM plugin_run_files m "
            f"LEFT JOIN plugin_file_outcomes o ON o.run_id=m.run_id "
            f"AND o.plugin_type=m.plugin_type AND o.file=m.file "
            f"LEFT JOIN raw r ON r.run_id=m.run_id AND r.file=m.file "
            f"WHERE m.run_id=%s AND m.plugin_type='stpcompare'",
            (run_id,),
        ).fetchone()
        return dict(row) if row else {}


def create_result_adapter(options: Mapping[str, Any]) -> StpCompareResultAdapter:
    return StpCompareResultAdapter(options.get("result_table"))
