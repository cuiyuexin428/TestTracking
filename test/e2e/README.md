# e2e 回归脚本

以下脚本覆盖框架主流程与 coverage 插件关键路径，均可重复执行（每次使用独立临时工作区与端口）：

| 脚本 | 覆盖 |
| --- | --- |
| `run_e2e.ps1` | demo 主流程：注册 → 请求任务 → 执行 → 上报 → run finished |
| `run_stop.ps1` | STOP 路径：运行中 stop → 回收/cancel → 再次 run 完成 |
| `run_deploy.ps1` | coverage 注册版本门禁：版本不符被拒并退出 → 修正版本后注册成功并完成 |
| `run_cov.ps1` | coverage 多波次 dry-run：80 文件 2 波次 greedy 结算 → selected.txt 80 行 |
| `run_recover.ps1` | 恢复路径：运行中 stop → 检查点落库 → 重启调度端自动恢复 → 再次执行完成（需本地 PostgreSQL，默认连接 127.0.0.1:5432/testtracking） |
| `run_reclaim.ps1` | 客户端离线回收：已完成任务不回收，其余任务进 priority 队列并重派 |
| `run_stop_wake.ps1` | STOP/WAKE + 幂等续跑：静默窗口不发任务，恢复后完成任务 |
| `run_cov_merge.ps1` | 真实 StepTest + OpenCppCoverage 波次 cov 合并与 wave summary（file/postgres 两种后端） |

## 运行

```powershell
powershell -ExecutionPolicy Bypass -File test\e2e\run_e2e.ps1 [-BinDir <构建输出目录>] [-Port 5557]
```

- `-BinDir` 缺省时按顺序探测 `out\build\x64-Debug\bin\Debug`、`out\build\Release\bin\Release`、`build\bin\Release`；
- 每个脚本结束时按日志断言（run finished / STOP / selected 行数），失败返回非零退出码；
- 工作区与日志写在 `%TEMP%\tt_e2e_<name>_<id>\`，脚本末尾打印路径，便于排查。

## 前置

- 先构建 `scheduler`、`client`、`demo`、`coverage` 四个目标；
- 各脚本不要同时跑（端口默认不同，可再传 `-Port` 区分）。
