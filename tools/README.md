# 第三方依赖构建

第三方库**不参与项目编译**，但跟随项目目录一起迁移。

## 目录布局

```
TestTracking\
  third_party\        第三方库安装产物（头文件 + 静态库 + CMake 配置）
  third_party_src\    第三方库源码（downloads 源码包 / src 解压源码 / build 构建目录）
  tools\build_third_party.ps1   独立构建脚本
```

## 构建

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_third_party.ps1
```

- 默认项目根目录 = 脚本所在目录的上一级；可用 `-ProjectRoot` 指定其他路径。
- 已安装的库自动跳过；`-Force` 重新下载、清理构建目录并重建。
- GitHub 连接不稳定时，可用 `-Mirror` 加镜像前缀重试，例如：
  `powershell -ExecutionPolicy Bypass -File .\tools\build_third_party.ps1 -Mirror https://ghproxy.net/`

## 包含的库

| 库 | 版本 | 形态 | 用途 |
| --- | --- | --- | --- |
| libzmq | 4.3.5 | 源码构建，静态库 | ZMQ 通信（ROUTER / DEALER） |
| abseil-cpp | 20230802.3 | 源码构建，静态库 | protobuf 的依赖（LTS 补丁版，含 hash 容器安全修复） |
| protobuf | 25.3 | 源码构建，静态库 + protoc | 消息协议定义（Abseil 版，最低支持 MSVC 2017 / C++14） |
| moodycamel | master | 单头文件 | 带超时的阻塞队列 |
| nlohmann/json | 3.11.3 | 单头文件 | 配置文件解析 |
| tinyxml2 | 随仓库 | 单头/源文件 | coverage 插件解析 cobertura XML |

## 拷贝到其他机器编译

整个 `TestTracking` 目录拷贝到其他机器即可直接编译：

- 项目根 `CMakeLists.txt` 用相对路径 `THIRD_PARTY_ROOT=<项目>/third_party` 引用安装产物，不依赖绝对路径；
- libzmq / protobuf / abseil 的 CMake 配置都是可重定位的（基于自身安装前缀解析）；
- 新机器无需联网、无需重新构建第三方库。
- PostgreSQL/libpq 是否可用取决于实际交付目录：完整离线包通常包含 `third_party/pgsql` 和 PostgreSQL 压缩包；纯 Git 工作副本可能没有这些大文件。缺少时需要安装 PostgreSQL，或设置 `PG_ROOT`，或在 CMake 配置时传 `-DPG_ROOT=<postgres-root>`。CMake 也会检查常见安装目录和已有开发目录。
- 首次使用数据库前执行 `tools\init_database.ps1` 创建 `testtracking` 数据库，或使用 `tools\setup_offline_server.ps1` 准备随包实例。
- 如需远程客户端直写本地数据库，先运行 `tools\setup_postgres_remote.ps1`，再执行 `tools\build_package.ps1`；后者会自动探测本机 IPv4 并写入 client 配置的 endpoint/db_conn。

`build_package.ps1` 不包含开发机资源路径默认值。完整客户端包请显式传入
`-StepTestExe`、`-DatasetDir`，coverage 还需 `-OpenCppCoverageDir`；相对路径按项目根目录
解析，资源会复制到包内 `testenv`，复制到其他机器后配置不引用源码目录。

`build_server_package.ps1` 默认携带独立 Python 和 psycopg，首次构建从 Python/PyPI
准备 `runtime/python` 缓存。离线时可显式传 `-PythonRuntimeDir`；
`-UseSystemPython` 才使用目标机 Python。`-IncludePostgresRuntime` 会携带完整的
PostgreSQL bin/lib/share 和初始化工具；运行机器仍需配置实际数据库连接。
包内 README 提供启动、数据库准备及结果界面的操作说明。

结果分析使用的 numpy、pandas、openpyxl、matplotlib 和 plotly 需要预先准备离线包。
联网构建机运行 `python tools/prepare_python_analysis_wheelhouse.py`，会为 CPython 3.12
Windows x64 下载固定版本 wheel、独立 `pip.pyz`，并生成 SHA256 清单。内网机器运行
`powershell -ExecutionPolicy Bypass -File tools/install_python_analysis_offline.ps1`，脚本只从
`runtime/python-analysis-wheelhouse` 安装到 `runtime/python`，校验导入后写入
`runtime/python/analysis-runtime-manifest.json`。wheelhouse 已由发布包携带时不需要联网。
TestTracking 与 DSH dist 一起复制到内网后，可运行
`setup_offline_dependencies.ps1 -DshBundleDir <dist> -LlmBaseUrl <内网/v1地址> -LlmModel <模型>`
一次安装并校验 Python 分析栈和 DSH 便携运行时。DSH 程序不访问 npm；聊天仍需要该
内网 OpenAI 兼容接口可达。

客户端发布包使用 Release x64 构建，默认输出 `dist\client_package`。脚本会从 Visual Studio
的 `VC\Redist\MSVC\<版本>\x64\Microsoft.VC*.CRT` 自动携带 VC++ DLL；也可显式传
`-VCRuntimeDir` 指定该目录。无需在目标机安装 Visual Studio。`-OnlyClient`、`-OnlyProgram`、
`-OnlyDataset` 每次只能选一个；程序单独更新时，已有客户端配置中的可执行文件名会同步更新。
`-OnlyClient` 保留已有部署配置与插件选择，只更新显式指定的配置字段。
两个打包脚本默认读取 `build\offline_verify\bin\Release`，与一键部署相同；使用其他构建目录
时必须传 `-BinDir` 指向本次产物，不再自动选择 `build\rt` 等历史目录。

源码复制到内网后，编辑 `tools/offline_deploy.json`，执行 `setup_offline_server.ps1`。
它默认编译 Release、初始化 PostgreSQL、生成数据 manifest 和客户端包、从本地 wheelhouse
安装 Python 分析依赖，并生成 `runtime/result_service.json` 和启动入口。
`plugin=coverage` 使用 `step_test_exe` 与 `opencppcoverage_dir`；
`plugin=stpcompare` 使用 `converter_exe`。两者的客户端包均携带完整 STP 数据。

运行 `runtime/start_server.bat` 启动调度器和网页，浏览器访问
`http://<服务器IP>:56330/`；`runtime/start_result_service.bat` 单独启动网页，
`runtime/stop_result_service.bat` 停止网页服务。调度器退出后，网页仍可查询数据库和生成报告。
配置页编辑的调度配置位于 `runtime/scheduler.json`，后续重新编译 Release 不会覆盖它；
客户端配置指向生成包的 `config/client.json`，使用 `-SkipPackage` 时指向明确标注的
`runtime/client.template.json`，正式模板默认关闭模拟执行，业务路径需补入。
日志集中在 `runtime/logs`。`management_port` 默认 5556，仅本机访问；
`result_port` 默认 56330，防火墙同时开放网页、任务和数据库端口。
已安装分析依赖时可用 `-SkipPythonAnalysis`；脚本不会在线补下载缺失依赖。
DSH 仍用 `setup_offline_dependencies.ps1` 配置内网模型接口；自定义 CLI 路径可写入
`offline_deploy.json` 的 `dsh_command`。再次部署会保留结果服务中的 AI 提供商配置。
一键部署自己初始化的 PostgreSQL 会记录准确的数据目录，联合启动入口在系统重启后会
按需启动这一实例；使用 `-SkipDatabase` 时不托管数据库启动。数据源不要设为
`runtime/dataset` 或其子目录；复用已准备的数据时使用 `-SkipDataset`，避免重新覆盖数据源。

如需在新机器上重建第三方库（例如换版本），先删除 `third_party_src\build` 再运行脚本，或直接加 `-Force`。

## 无网络环境重建

- `third_party_src\downloads\` 已包含全部源码包和单头文件；脚本检测到本地已存在时不会联网下载，可完全离线编译。
- 新机器只需安装 VS2022 + CMake，运行脚本即用本机工具链编译出 VS2022（v143）版本的库。
- 重建时建议加 `-Force`（或先删除 `third_party_src\build\`），避免旧机器 VS 路径残留在 CMake 缓存中导致工具链检测失败。
- 若 `third_party\` 安装产物随目录一起拷贝，则无需编译，直接 `cmake` 构建项目即可；若新机器是 VS2017 等旧工具链，建议在新机器上重新编译（各库最低支持 VS2017）。
