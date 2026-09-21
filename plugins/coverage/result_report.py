"""Coverage compact-set report backed only by durable database results."""
from base64 import b64encode
from html import escape


def _decode_points(value):
    data = bytes(value)
    if len(data) % 8:
        raise ValueError("coverage_file_points contains a malformed point set")
    return {int.from_bytes(data[offset:offset + 8], "little")
            for offset in range(0, len(data), 8)}


def _select_compact_cover(rows):
    """Select a deterministic greedy cover from successful raw results."""
    candidates = []
    for row in rows:
        if row["status"] != "success" or row["point_data"] is None:
            continue
        candidates.append({"file": row["file"], "size_bytes": row["size_bytes"],
                           "points": _decode_points(row["point_data"])})
    selected = []
    covered = set()
    remaining = {item["file"]: item for item in candidates}
    while remaining:
        ranked = []
        for item in remaining.values():
            new_points = item["points"] - covered
            size = item["size_bytes"]
            ranked.append((-len(new_points), size is None, size or 0, item["file"],
                           item, new_points))
        _, _, _, _, best, new_points = min(ranked, key=lambda value: value[:4])
        if not new_points:
            break
        selected.append({"file": best["file"], "size_bytes": best["size_bytes"],
                         "new_points": len(new_points)})
        covered.update(best["points"])
        del remaining[best["file"]]
    return selected, covered


def _size_label(value):
    if value is None:
        return "未知"
    units = ("B", "KiB", "MiB", "GiB", "TiB")
    number = float(value)
    unit = units[0]
    for unit in units:
        if number < 1024 or unit == units[-1]:
            break
        number /= 1024
    return f"{int(number)} {unit}" if unit == "B" else f"{number:.2f} {unit}"


def render(connection, context):
    run_id = context["run_id"]
    run = connection.execute(
        "SELECT run_id, expected_files, status, dataset_version, program_version "
        "FROM plugin_runs WHERE run_id=%s AND plugin_type='coverage'", (run_id,)
    ).fetchone()
    if not run:
        raise ValueError("没有找到 coverage 的持久运行记录")

    raw_exists = connection.execute(
        "SELECT to_regclass('coverage_file_points') AS relation"
    ).fetchone()["relation"]
    columns = {row["attname"] for row in connection.execute(
        "SELECT attname FROM pg_attribute WHERE attrelid=to_regclass('plugin_run_files') "
        "AND attnum>0 AND NOT attisdropped"
    ).fetchall()}
    size = "m.size_bytes" if "size_bytes" in columns else "NULL::bigint"
    digest = "m.content_sha256" if "content_sha256" in columns else "NULL::text"
    point_data = "p.points" if raw_exists else "NULL::bytea"
    raw_join = ("LEFT JOIN coverage_file_points p ON p.run_id=m.run_id AND p.file=m.file "
                if raw_exists else "")
    rows = connection.execute(
        "SELECT m.file, o.status, o.detail, " + size + " AS size_bytes, " +
        digest + " AS content_sha256, " + point_data + " AS point_data "
        "FROM plugin_run_files m LEFT JOIN plugin_file_outcomes o "
        "ON o.run_id=m.run_id AND o.plugin_type=m.plugin_type AND o.file=m.file " +
        raw_join + "WHERE m.run_id=%s AND m.plugin_type='coverage' ORDER BY m.file",
        (run_id,),
    ).fetchall()

    expected = int(run["expected_files"])
    valid = [row for row in rows if row["status"] == "success" and row["point_data"] is not None]
    success = len(valid)
    failed = sum(row["status"] == "failed" for row in rows)
    missing = max(0, expected - success - failed)
    complete = len(rows) == expected and success == expected
    selected, covered = _select_compact_cover(rows)
    known_selection_size = all(item["size_bytes"] is not None for item in selected)
    selection_size = sum(item["size_bytes"] for item in selected) if known_selection_size else None
    readiness = ("原始数据完整，精简集合可用于后续处理" if complete else
                 "原始数据尚不完整，当前精简集合会随结果入库而变化")

    cards = [("计划文件", expected), ("原始结果齐全", success),
             ("精简集合文件", len(selected)), ("精简集合大小", _size_label(selection_size)),
             ("覆盖点数", len(covered)), ("待补齐", missing)]
    metrics = "".join('<div class="metric"><span>' + escape(label) + '</span><strong>' +
                      escape(str(value)) + '</strong></div>' for label, value in cards)
    statuses = [("原始结果齐全", success, "#23856d"), ("执行失败", failed, "#c14f5d"),
                ("待补齐", missing, "#718096")]
    bars = "".join('<div class="bar-row"><span>' + escape(label) + '</span><div class="track">'
                   '<i style="width:' + str(100 * count / max(1, expected)) + '%;background:' + color +
                   '"></i></div><b>' + str(count) + '</b></div>' for label, count, color in statuses)

    selected_files = "\n".join(item["file"] for item in selected) + ("\n" if selected else "")
    download = "data:text/plain;charset=utf-8;base64," + b64encode(selected_files.encode("utf-8")).decode("ascii")
    selection_rows = "".join(
        "<tr><td>" + str(index) + "</td><td>" + escape(item["file"]) + "</td><td>" +
        str(item["new_points"]) + "</td><td>" + escape(_size_label(item["size_bytes"])) + "</td></tr>"
        for index, item in enumerate(selected, 1))
    if not selection_rows:
        selection_rows = '<tr><td colspan="4" class="muted">当前结果没有可新增的覆盖点</td></tr>'

    file_rows = ""
    selected_names = {item["file"] for item in selected}
    for row in rows:
        points = len(_decode_points(row["point_data"])) if row["point_data"] is not None else None
        status = ("原始结果齐全" if row["status"] == "success" and points is not None
                  else "执行失败" if row["status"] == "failed" else "待补齐")
        values = [row["file"], "是" if row["file"] in selected_names else "否", status,
                  points if points is not None else "—", _size_label(row["size_bytes"]),
                  row["content_sha256"] or "—", row["detail"] or ""]
        file_rows += "<tr>" + "".join("<td>" + escape(str(value)) + "</td>" for value in values) + "</tr>"

    file_filter = escape(str(context.get("parameters", {}).get("file_filter", "")), quote=True)
    provisional = "" if complete else '<span class="tag">临时结果</span>'
    return """<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Coverage 精简报告</title>
<style>
*{box-sizing:border-box}body{margin:0;padding:24px;background:#f4f7fb;color:#24324a;font:15px/1.6 system-ui,sans-serif}h1{font-size:24px;margin:0 0 6px;letter-spacing:0}h2{font-size:18px}.muted{color:#66758d;overflow-wrap:anywhere}.panel{background:#fff;border:1px solid #dce4ef;border-radius:8px;padding:20px;margin:18px 0}.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:14px;margin-top:18px}.metric{border-left:3px solid #3568b8;padding-left:13px}.metric span{display:block;color:#66758d}.metric strong{font-size:23px}.bar-row{display:flex;align-items:center;gap:12px;margin:9px 0}.bar-row span{width:110px}.bar-row b{min-width:35px}.track{height:12px;background:#e9eef5;flex:1;border-radius:6px;overflow:hidden}.track i{display:block;height:100%}.actions{display:flex;align-items:center;gap:12px;flex-wrap:wrap}.download{display:inline-flex;padding:9px 13px;border-radius:6px;background:#245fae;color:#fff;text-decoration:none;font-weight:600}.tag{margin-left:8px;padding:2px 7px;border-radius:4px;background:#fff0c7;color:#7b5700;font-size:12px}input{padding:9px 11px;border:1px solid #c6d2e1;border-radius:6px;width:min(100%,420px);font:inherit}.table-wrap{overflow:auto}table{border-collapse:collapse;width:100%;min-width:700px}th,td{text-align:left;padding:10px;border-bottom:1px solid #e8edf4;vertical-align:top}td{overflow-wrap:anywhere}th button{border:0;background:none;font:inherit;color:inherit;cursor:pointer;padding:0}td:nth-child(2){max-width:430px}@media(max-width:600px){body{padding:12px}.panel{padding:14px}.metrics{grid-template-columns:repeat(2,minmax(0,1fr))}h1{font-size:21px}}
</style><h1>Coverage 精简报告</h1><div class="muted">运行：""" + escape(run_id) + """</div>
<section class="panel"><strong>""" + readiness + provisional + """</strong><div class="metrics">""" + metrics + """</div></section>
<section class="panel">""" + bars + """</section>
<section class="panel"><div class="actions"><div><h2>建议 STP 集合</h2><p class="muted">按新增覆盖点贪心选择；新增点相同时优先较小文件，再按路径排序。该集合确定且实用，但不保证数学上的全局最少文件数。</p></div><a class="download" download="coverage-selected-stp.txt" href=""" + download + """">下载文件清单</a></div>
<div class="table-wrap"><table><thead><tr><th>顺序</th><th>STP 文件</th><th>新增覆盖点</th><th>文件大小</th></tr></thead><tbody>""" + selection_rows + """</tbody></table></div></section>
<section class="panel"><h2>逐文件原始结果</h2><input id="filter" aria-label="筛选文件" placeholder="筛选文件、状态或摘要" value=""" + '"' + file_filter + '"' + """>
<p class="muted" id="count"></p><div class="table-wrap"><table><thead><tr><th><button data-column="0">文件 ↕</button></th><th>已选择</th><th>结果状态</th><th>覆盖点数</th><th>文件大小</th><th>SHA-256</th><th>说明</th></tr></thead><tbody>""" + file_rows + """</tbody></table></div></section>
<script>const body=document.querySelectorAll('tbody')[1],filter=document.querySelector('#filter');function applyFilter(){let shown=0;for(const row of body.rows){row.hidden=!row.textContent.toLowerCase().includes(filter.value.toLowerCase());if(!row.hidden)shown++;}document.querySelector('#count').textContent=`显示 ${shown} / ${body.rows.length} 个文件`;}filter.addEventListener('input',applyFilter);applyFilter();for(const button of document.querySelectorAll('[data-column]'))button.addEventListener('click',()=>{const i=Number(button.dataset.column),direction=button.dataset.direction==='asc'?-1:1;button.dataset.direction=direction===1?'asc':'desc';Array.from(body.rows).sort((a,b)=>a.cells[i].textContent.localeCompare(b.cells[i].textContent,'zh-CN',{numeric:true})*direction).forEach(row=>body.appendChild(row));});</script></html>"""
