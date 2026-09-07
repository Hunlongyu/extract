# Extract

Windows 原生安装包解包工具，优先覆盖近几年发布的安装包。

技术栈：**C++20 自研代码 + C 基础库 + CMake + Win32**。C 编译标准设为 C17，不为混合语言而刻意拆分自研模块。

更新日期：2026-09-07。当前为 **0.6.0 MSI + CAB + Burn + Inno + NSIS + ZIP/7z 版**，支持已识别内嵌安装包及 NSIS 归档载荷自动展开，并自动记录详细日志。安装包结构解析、目录规划、提取和校验由本项目实现；压缩和 7z 归档底层使用 Windows CAB API、静态链接的 C 基础库。没有外部解包程序依赖。

| 格式 | 当前已验证范围 |
| --- | --- |
| MSI | 单个/多个独立 CAB，内嵌/外置 CAB，松散/混合源文件，长短源名称；空 File 表仅输出说明清单 |
| CAB | 标准 Microsoft CAB 1.3；无压缩、MSZIP、LZX 已验证；独立文件与 PE 附加区 CAB |
| WiX Burn | 二进制布局 2、CAB 容器、v3/v4 清单命名空间；内嵌/分离容器与本地外置载荷，SHA-1/256/512 校验；缺失在线载荷报告部分完成 |
| Inno Setup | 官方 6.2.2、6.5.0、6.5.4、6.6.1、6.7.3、7.1.0 生成的内嵌包；7.1 的 x86/x64 安装器 |
| Inno 压缩 | 无压缩、Deflate、bzip2、LZMA、LZMA2，普通块和 solid 流 |
| NSIS | 官方 3.11 编译器生成的 Unicode 包、用户 XnView 外壳的 3.12 Unicode 结构；当前验收为 x86 安装器，可含 x64 应用载荷 |
| NSIS 压缩 | 无压缩、raw Deflate、NSIS bzip2、LZMA；逐块与 Solid |
| NSIS 封装 | 支持替换 BrandingText 的 Electron-builder 包；显式目录赋值的控制流合流支持 Pebble/Tauri 样本 |
| ZIP / ZIP64 | stored、Deflate、bzip2，UTF-8 / CP437 / Unicode 路径扩展、数据描述符 |
| 7z | Copy、LZMA、LZMA2，普通/固实块、BCJ/BCJ2；包头、固实块及存在的文件 CRC |
| PE SFX | PE 附加区中的 ZIP/7z/CAB；静态读取归档，不执行自解压配置 |

Inno 按精确数据版本 `6.1.0 (u)`、`6.5.0`、`6.5.2`、`6.6.1`、`6.7.0`、`7.0.0.3` 选择布局，不把编译器产品版本区间当作全覆盖保证。

## 使用

将一个或多个受支持的安装包或 ZIP/7z/CAB 归档拖到 `Extract.exe` 上。文件输出到安装包旁边的 `<名称>_extracted`，同名时自动加序号。完成后提交静音系统通知；点击单个成功任务打开输出目录，批次、部分完成或失败任务打开记录目录。

```powershell
Start-Process .\Extract.exe -ArgumentList '--quiet "D:\Downloads\app.msi"' -WindowStyle Hidden -Wait
Start-Process .\Extract.exe -ArgumentList '--output "D:\Unpacked" "D:\Downloads\app.msi"' -WindowStyle Hidden -Wait
```

`--output` 指向已存在的父目录。`--list` 输出一个安装包的 JSON 文件清单，不生成提取结果；NSIS 需要解码载荷计算大小，Solid 包会使用自动清理的临时磁盘缓存。`--quiet` 关闭本次通知。其它命令包括 `--open-last-result`、`--repair-notifications`、`--unregister-notifications`、`--help`、`--version`。

支持中文和空格路径、多文件批次、空文件、同路径和文件/目录冲突保留。`_extract-report.json` 记录格式、数据版本、原始表达式、条件、实际路径、大小和哈希结果。Inno 文件逐个比对包内 SHA-1 或 SHA-256，并记录 `sourceHashAlgorithm`；所有输出另算 SHA-256。MSI 在可用时检查 MsiFileHash。条件文件全部保留在静态清单中，不执行安装条件。

Inno 的 `{app}` 等根目录保留为输出内的逻辑目录。无法静态还原的动态路径放入 `_unresolved`，报告部分完成并返回 299。安装时生成的卸载程序不属于静态载荷，在报告中说明。

NSIS 的 `$INSTDIR`、`$PLUGINSDIR`、`$TEMP`、`$EXEDIR` 映射为 `app`、`plugins`、`temp`、`exedir`。在有 CRC 的包上验证包内 CRC32；`packageCrcPresent` / `packageCrcVerified` 单独记录它，与文件 SHA-256 区分。跨分支或动态路径无法确定时放入 `_unresolved`；只有已按结构识别的卸载器存在路径问题时仍报告成功，其它未解析载荷返回 299。相同路径、相同数据块的重复 File 指令合并，不同内容保留为 `_variants`。

**默认继续展开已识别的内嵌 MSI / Inno / NSIS / Burn 安装包和 SFX。** 原内层包保留，子结果位于本层输出目录；`--list` 仍只列一层。用户 XnView 样本已通过 NSIS → Inno 6.5.0 两层提取，得到 1080 个应用文件，主程序在 `XnViewMP-win-x64_extracted/app/xnviewmp.exe`。单一 NSIS 外壳的通知结果直接指向内层应用目录。

MSI 外置 CAB 放在 MSI 同目录，松散文件按源目录布局提供。Burn 的引导文件输出到 `bootstrapper`，载荷输出到 `packages`，同名条件变体分别保留。Burn 清单声明的包外文件缺失时保留已有文件，记录 `contentComplete: false` 并返回 299；不自动下载。v3 已用官方 Python 3.13.7 验证，v4 命名空间与 SHA-512 用已知内容的结构样本验证，不能据此保证所有 WiX 产品版本。

报告的 `nestedPackages` 记录每个识别出的内层包与结果；`treeFileCount`、`treeTotalBytes` 记录累计输出。NSIS 非 `app` 根下及 CAB 内的 ZIP/7z/CAB 载荷自动展开，普通应用目录里的归档资源保留为数据。嵌入的 NSIS 卸载器作为文件保留，标记 `preserved`，不递归。识别出的内层不支持、损坏或达到限制时，保留已完成的外层，返回 **299 / partial**。

0.5.1 按应用载荷判断是否完成：卸载器不属于必需的应用文件。已识别的卸载器作为辅助文件保留，其原始路径未还原不再使任务变为 partial；报告保留 `pathResolved` 和 `role`，用 `requiredPathsResolved` 区分需要还原的载荷路径。识别依据为 NSIS 包头中的卸载器标志，不凭 `Uninstall.exe` 等文件名猜测。安装时生成的卸载器仍无需生成。

用户 ECHO NEXT、vic-diary 的 `app-64.7z` 提取完成后，通知直接指向 `app-64_extracted`。Pebble 仍输出到 `app/pebble.exe`。目录传播只根据显式 NSIS 赋值和分支合流，不模拟插件任意修改变量，也不生成下载文件、脚本写入内容或卸载器。ANSI、改版字节码和未列明架构仍未覆盖。

尚不支持加密/多卷归档、其它压缩方法、RAR、跨卷续接 CAB、InstallShield 私有 CAB、MSI 管理映像和 MST/MSP，以及 Inno 外置卷、加密、在线载荷和其它数据版本。只接受无重解析点的本地磁盘路径；每个输入包上限 512 MiB，每个根任务各层累计最多 10,000 个文件、8 GiB 输出、32 个包和 4 层（含根层）。原始内层包也计入字节预算，MSI/Burn 因可能依赖本地外置文件不复用仅按包哈希缓存的结果。Inno 元数据每块 64 MiB，NSIS 元数据 64 MiB / 指令 100,000 条，流式 LZMA 字典 256 MiB，MSI 单个 CAB 256 MiB、外置 CAB 累计 512 MiB。Burn 最多 128 个容器，清单 8 MiB，外置输入累计 512 MiB。CAB 解码分配预算 64 MiB、文件名总计 8 MiB。ZIP 中央目录和 7z 常驻元数据上限 64 MiB，7z 辅助解码分配 256 MiB；固实块用受目录锁保护的临时磁盘映射，另计入最多 8 GiB 的缓存空间。缓存与最终输出同时存在，工作进程隔离和硬超时尚未实现。归档的空目录不单独创建，链接和特殊文件拒绝提取。

解包得到的是包内静态文件和逻辑目录，不执行安装动作，也不保证软件可以直接便携运行。系统设置可能抑制通知横幅；本地记录位于 `%LOCALAPPDATA%\Extract\jobs\`。首次通知会建立当前用户的快捷方式与通知协议注册，移动便携目录后可运行 `--repair-notifications` 更新路径。

## 日志与排查

每次运行自动在 **`Extract.exe` 同级的 `log` 目录**写入 UTF-8 日志；目录不可读写或被占用等原因导致无法创建日志时，回退到 **`%LOCALAPPDATA%\Extract\log`**。日志按运行和分段分别保存，多次拖拽不会混写。`--quiet` 只关闭通知，仍记录日志；`--list` 保持标准输出为 JSON，详细过程另写日志。

单文件最多 **2 MiB**，目录总量 **10 MiB**，最多 **200 个文件**，保留 **30 天**。启动、轮转、退出及持续写入期间每隔 60 秒检查清理；按最旧优先删除本工具的非活动日志。并发活动文件或无法删除的旧文件可能暂时超过目录限额，后续检查继续收敛。程序退出后不驻留后台清理。

日志包含时间、会话、完整输入路径、内层包路径、阶段、文件、压缩方式、校验、输出位置、耗时和错误原因/系统错误码。失败时打开任务记录中的 `summary.txt`，按“详细日志”找到本次 `extract-日期-时间-会话-分段.log`，重点查看 `input.failed`、`parse.failed`、`nested.failed`、`step.aborted` 或 `burn.payload_missing`。日志为每行一个 JSON 对象的文本，记事本也可直接查看；不会自动上传文件内容或日志。两处日志目录均不可用时不因此中断提取。详细策略见 [日志实现与排查](docs/15-logging.md)。

## 构建

要求 Windows、Visual Studio C++ 桌面开发工具（支持预览版）、CMake 3.25+、Ninja。测试使用 PowerShell 7、Python 3.10+；格式测试分别需要官方 ISCC / makensis 编译器，缺少对应编译器会标记跳过。脚本自动发现 Visual Studio 并初始化 x64 MSVC 环境。发布程序不依赖这些测试工具。

```powershell
.\scripts\build.ps1 -Configuration Debug -Test
.\scripts\build.ps1 -Configuration Release -Test -Install
# 测试用 NSIS 编译器在便携目录时：
.\scripts\build.ps1 -Configuration Debug -Test -NsisCompiler 'D:\Tools\NSIS\Bin\makensis.exe'
```

- 编译产物：`out/build/win-x64-release/bin/Extract.exe`
- 便携目录：`out/dist/win-x64-release/`
- 详细步骤与当前行为：[构建与开发](docs/05-build-and-development.md)

自动测试逐文件对照打包前的原始内容，覆盖路径、条件变体、压缩流、损坏数据和批次结果。真实样本包含 PuTTY 0.85 MSI 和官方 Inno 安装包。详细结果见 [MSI 报告](docs/06-msi-first-slice.md)与 [Inno 报告](docs/07-inno-implementation.md)；基础库来源与许可见 [third_party](third_party/README.md)。

## 已确认的要求

- 主程序、安装包识别、格式解析与提取流程由本项目实现。
- 允许使用原生 C/C++ 压缩与归档基础库；无需自行发明 ZIP、LZMA 等算法。
- 参考 UniExtract、UniExtract 2、lessmsi 的设计和源码，不以调用这些程序作为产品实现。
- 无主界面，文件拖到程序图标上即可处理；成功失败用右下角系统通知。
- 便携文件夹发布；保留原生程序、必要基础库和说明文件。
- 优先近期安装包，不为老旧安装器维护大量历史兼容分支。

## 推荐范围

“近几年”暂按 2021–2026 年发布的软件安装包建立样本集，以当前稳定版本为重点，不按文件时间戳拒绝输入。

核心阶段：Inno Setup 6.2–6.7 与 7.0/7.1、NSIS 3.x 和近期 Electron/NSIS 封装、MSI；配套实现 ZIP/7z/CAB 读取。Burn 首批结构已接入；后续扩展 MSIX/APPX、Velopack/Squirrel 的近期离线包。

以上是开发目标，不是已验证支持列表。模块按实际二进制结构和样本验收，不能只凭产品版本号承诺全部变体。

## 文档

| 文档 | 内容 |
| --- | --- |
| [01 可行性与技术选型](docs/01-feasibility.md) | 自主实现边界、近期格式、参考来源、基础库选择 |
| [02 产品需求](docs/02-requirements.md) | 已确认需求、拖拽与输出、通知、兼容边界 |
| [03 技术设计](docs/03-technical-design.md) | 原生模块、统一文件模型、MSI/Inno/NSIS 解析、嵌套载荷 |
| [04 开发计划与验收](docs/04-roadmap-and-acceptance.md) | 按格式交付、样本矩阵、近期覆盖率和工期估算 |
| [05 构建与开发](docs/05-build-and-development.md) | 工具链、构建命令、目录、初始化范围和验证方法 |
| [06 MSI 实现与验证](docs/06-msi-first-slice.md) | 首版实现、限制、自动测试和近期真实样本结果 |
| [07 Inno 实现与验证](docs/07-inno-implementation.md) | 精确结构、五种压缩、条件变体、版本矩阵与真实样本 |
| [08 Textify 兼容修复](docs/08-textify-compatibility.md) | loader v1、Unicode 6.1.0 数据结构与 SHA-1 验证 |
| [09 NSIS 实现与验证](docs/09-nsis-implementation.md) | 块/字符串/文件指令、三种压缩、Solid、XnView 两层提取与限制 |
| [10 内嵌包展开与 Inno 6.5.0](docs/10-nested-extraction.md) | 自动展开、累计预算、部分完成状态、XnView 应用内容验收 |
| [11 常见封装与样本验收](docs/11-common-wrappers.md) | ZIP/7z/SFX、Electron/Tauri、目录内 5 个安装包的实际结果 |
| [12 本地工具清点与实现优先级](docs/12-extractor-inventory-and-priorities.md) | 两个 UniExtract 工具目录的逐项用途、插件与依赖、近期格式开发建议 |
| [13 MSI/CAB 优化与 Burn 实现](docs/13-msi-cab-burn.md) | 多媒体和松散源、独立 CAB、Burn 容器映射、缺失载荷和验证边界 |
| [14 卸载辅助文件与完成状态](docs/14-uninstaller-completion.md) | 卸载器不影响成功判定、应用目录定位、真实样本和反例验证 |
| [15 日志实现与排查](docs/15-logging.md) | 同级目录与 AppData 回退、步骤记录、轮转清理、失败定位和测试 |

后续继续扩展经过验证的现代封装变体，以及 worker 与硬超时。
