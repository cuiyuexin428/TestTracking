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

如需在新机器上重建第三方库（例如换版本），先删除 `third_party_src\build` 再运行脚本，或直接加 `-Force`。

## 无网络环境重建

- `third_party_src\downloads\` 已包含全部源码包和单头文件；脚本检测到本地已存在时不会联网下载，可完全离线编译。
- 新机器只需安装 VS2022 + CMake，运行脚本即用本机工具链编译出 VS2022（v143）版本的库。
- 重建时建议加 `-Force`（或先删除 `third_party_src\build\`），避免旧机器 VS 路径残留在 CMake 缓存中导致工具链检测失败。
- 若 `third_party\` 安装产物随目录一起拷贝，则无需编译，直接 `cmake` 构建项目即可；若新机器是 VS2017 等旧工具链，建议在新机器上重新编译（各库最低支持 VS2017）。
