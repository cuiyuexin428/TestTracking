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
        return dict(row) if row else {}


def create_result_adapter(options: Mapping[str, Any]) -> StpCompareResultAdapter:
    return StpCompareResultAdapter(options.get("result_table"))
