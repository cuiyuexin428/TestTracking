# e2e 回归脚本

三个脚本覆盖框架主流程，均可重复执行（每次使用独立临时工作区与端口）：

| 脚本 | 覆盖 |
| --- | --- |
| `run_e2e.ps1` | demo 主流程：注册 → 请求任务 → 执行 → 上报 → run finished |
| `run_stop.ps1` | STOP 路径：运行中 stop → 回收/cancel → 再次 run 完成 |
| `run_cov.ps1` | coverage 多波次：版本不符被拒 → 共享目录部署 → 重注册 → 80 文件 2 波次 greedy 结算 → selected.txt 80 行 |

## 运行

```powershell
powershell -ExecutionPolicy Bypass -File test\e2e\run_e2e.ps1 [-BinDir <构建输出目录>] [-Port 5557]
```

- `-BinDir` 缺省时按顺序探测 `out\build\x64-Debug\bin\Debug`、`out\build\Release\bin\Release`、`build\bin\Release`；
- 每个脚本结束时按日志断言（run finished / STOP / selected 行数），失败返回非零退出码；
- 工作区与日志写在 `%TEMP%\tt_e2e_<name>_<id>\`，脚本末尾打印路径，便于排查。

## 前置

- 先构建 `scheduler`、`client`、`demo`、`coverage` 四个目标；
- 三个脚本不要同时跑（端口默认 5557，可传 `-Port` 区分）。
