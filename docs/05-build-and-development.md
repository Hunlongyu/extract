# 构建与开发

更新日期：2026-09-07。适用于 0.6.0 MSI + CAB + Burn + Inno + NSIS + ZIP/7z/SFX 版。

## 工具链与构建

自有代码使用 C++20，基础库使用 C17 编译。支持 Windows x86、x64、ARM64 构建预设，默认 x64，使用 MSVC、Windows SDK、CMake 3.25+ 和 Ninja；MSI 开发测试要求 PowerShell 7（`pwsh`），Inno 测试另需 Python 3.10+ 与官方 ISCC。CRT 和基础库静态链接，发布程序不依赖这些开发工具。

在仓库根目录执行：

```powershell
.\scripts\build.ps1 -Configuration Debug -Test
.\scripts\build.ps1 -Configuration Release -Test -Install
```

脚本通过 `vswhere -prerelease` 发现包含目标架构 C++ 工具的 Visual Studio，以 `vcvarsall.bat` 初始化当前进程环境，优先使用 Visual Studio 的 CMake/Ninja。它不会修改用户或系统环境变量，也不固定 MSVC 版本目录。`-Architecture x86|x64|arm64` 选择架构，各架构使用独立的构建及安装目录，配置时核对编译器的真实架构。`-Test` 运行 CTest 并写入 `test-results.xml`；`-Install` 生成便携目录。ARM64 交叉编译需安装 MSVC ARM64 工具和 Windows SDK，使用 `-WithoutTests`，不在 x64 主机上运行 ARM64 测试。需要重建错误的 CMake 缓存时加 `-Fresh`；不要在未初始化 MSVC 的终端直接用预设重配已有缓存。详见 [发布流程](16-release.md)。

链接保留 `/guard:cf`，并使用 `/INCREMENTAL:NO` 完整生成 CFG 目标表。本机 MSVC 预览工具链曾在增量链接后遗漏 CAB 析构跳板，触发 `0xC0000409 / FAST_FAIL_GUARD_ICALL_CHECK_FAILURE`；完整链接后同一回归包恢复预期的 299 部分完成，不关闭 CFG 来绕过检查。

NSIS 测试使用官方 `makensis.exe` 生成惰性包，参数示例：

```powershell
.\scripts\build.ps1 -Configuration Debug -Test -NsisCompiler 'D:\Tools\NSIS\Bin\makensis.exe'
```

此参数存入 `EXTRACT_NSIS_COMPILER` CMake 缓存；未指定时测试查找 `Program Files (x86)/NSIS/Bin/makensis.exe`。缺少编译器时对应 CTest 标记 skipped，不代表格式回归通过。编译器与测试样本不随产品分发。

归档测试另外使用已核对来源的官方 LZMA SDK `7zr.exe`，生成已知内容的 7z 和 SFX 数据。通过 `-SevenZipTestTool` 设置 `EXTRACT_7Z_TEST_TOOL` 后添加 `archive_integration`；未提供时该项不注册，不能算完整归档回归。0.5.0 新增不依赖安装器编译器的 `cab_burn_integration`，0.6.0 新增 `log_integration`；工具齐备时共 8 项 CTest：

```powershell
.\scripts\build.ps1 -Configuration Release -Test -Install `
    -NsisCompiler 'D:\Tools\NSIS\Bin\makensis.exe' `
    -SevenZipTestTool 'D:\Tools\lzma2603\bin\x64\7zr.exe'
```

已初始化 MSVC 环境时也可手动运行：

```powershell
cmake --preset win-x64-release
cmake --build --preset win-x64-release --parallel
ctest --test-dir out/build/win-x64-release --output-on-failure
cmake --install out/build/win-x64-release
```

Inno 测试默认寻找 `Program Files (x86)/Inno Setup 6/ISCC.exe`，也可在配置时指定：

```powershell
cmake --preset win-x64-release -DEXTRACT_INNO_COMPILER="D:/Tools/InnoSetup/ISCC.exe"
python tests/inno_integration.py --executable out/build/win-x64-release/bin/Extract.exe `
    --work-root out/inno-tests --compiler "D:/Tools/InnoSetup/ISCC.exe"
```

没有 Python 时 CMake 不添加 Python 格式测试；没有 ISCC 或 makensis 时相应测试以 77 标记为跳过，嵌套测试需要两者。跳过不等于通过。完整验收可分别指定官方 6.2.2、6.5.0、6.5.4、6.6.1、6.7.3、7.1.0 编译器，后者自动生成 x86/x64 两种安装器。测试不会下载或安装编译器，也不会运行生成的安装器。6.2.2 的开发工具来源及特殊加密反例见 [Textify 修复记录](08-textify-compatibility.md)。

| 路径 | 用途 |
| --- | --- |
| `out/build/win-x64-debug/bin/Extract.exe` | Debug 程序 |
| `out/build/win-x64-release/bin/Extract.exe` | Release 程序 |
| `out/dist/win-x64-release/` | 便携程序、说明与文档 |
| `out/build/<预设>/compile_commands.json` | 编译数据库 |
| `out/build/<预设>/test-fixtures/<随机编号>/` | 自制 MSI 和测试结果 |
| `out/build/<预设>/inno-fixtures/<随机编号>/` | 默认 Inno 测试包、输出与 `validation.json` |
| `out/samples/` | 手动验证的真实样本，不进入 Git |

## 使用与退出码

GUI 子系统程序不创建控制台。Explorer 拖拽会把路径作为启动参数传入；终端场景复用父控制台或重定向输出。命令行自动化必须等待程序退出。

```powershell
$process = Start-Process .\out\dist\win-x64-release\Extract.exe `
    -ArgumentList '--quiet "D:\Downloads\app.msi"' `
    -WindowStyle Hidden -Wait -PassThru
$process.ExitCode
```

| 命令 | 行为 |
| --- | --- |
| `<安装包路径...>` | 顺序处理，输出在源文件旁，结束后提交通知 |
| `--output <父目录> <安装包路径...>` | 使用已存在的本地父目录 |
| `--list <一个安装包>` | 输出 JSON 清单和详细过程日志，不生成提取结果或 jobs 任务摘要；Inno 检查元数据，NSIS 检查包 CRC 并解码载荷计算大小，Solid 使用自动清理的磁盘缓存；不声称完成逐文件哈希核对 |
| `--quiet <安装包路径...>` | 提取及记录日志，但不注册/发送通知 |
| `--open-last-result` | 打开最近任务保存的结果目录 |
| `--repair-notifications` | 将当前用户通知注册更新到本 EXE 的路径 |
| `--unregister-notifications` | 清理本程序快捷方式、协议与 AUMID 注册和通知历史，保留任务记录 |
| `--help` / `--version` | 输出使用说明 / 版本 |

`--` 之后的参数只按文件路径处理。`--list` 不接受多文件或 `--output`。

| 退出码 | 含义 |
| --- | --- |
| 0 | 本次操作成功 |
| 5 | 拒绝不安全路径或非法通知任务参数 |
| 13 | 数据/表关系/压缩流/内容校验异常 |
| 30 | 文件、目录、任务记录等 I/O 失败；具体系统错误见日志 |
| 50 | 当前结构不支持 |
| 160 | 启动参数无效；无参数启动也返回此值并提示拖入文件 |
| 223 | 达到文件数、输入/输出大小等上限 |
| 299 | 批次成功与失败混合，必需载荷动态路径，或识别出的内层包不支持/损坏/超限等部分完成结果；已识别卸载器的路径问题不计入 |
| 574 | 内部异常 |

全部失败的批次返回首个失败类型。解包后通知或最终日志写入失败不推翻已提交的解包结果。

## 源码与验证

| 模块 | 内容 |
| --- | --- |
| `src/app/main.cpp` | 宽字符参数、逐文件任务、批次状态 |
| `src/core/types.*` | 文件清单、预算、错误与 JSON 报告 |
| `src/core/package.*` | 格式入口、共享同名与前缀冲突规划 |
| `src/formats/msi.*` | 只读数据库、表关联、目录与媒体规划 |
| `src/formats/cab.*` | 独立标准 CAB、PE 附加区 CAB、共享路径规划 |
| `src/formats/burn.*` | Burn 版本 2、XML 清单、容器和载荷映射、外置来源与哈希 |
| `src/formats/pe.*` | PE32/PE32+ 资源目录定位与边界检查 |
| `src/formats/inno.*` | 精确数据版本、文件表、固实块、路径表达式与包内摘要校验 |
| `src/formats/nsis.*` | PE 附加区、包 CRC、块/指令/语言/字符串表、基本块路径传播、Solid 缓存 |
| `src/formats/archive.*` | ZIP/ZIP64 自研解析、7z C 基础层适配、SFX 附加区、CRC 校验与磁盘缓存 |
| `src/codecs/cab.*` | FDI 受控回调、内容校验 |
| `src/codecs/stream.*` | 有界 LZMA/LZMA2/Deflate/bzip2 解码 |
| `src/codecs/nsis_bzip.*` | 隔离 NSIS bzip2 修改格式与标准 bzip2 的 C 接口 |
| `src/io/input.*` | 只读输入映射与有界小端读取 |
| `src/io/output.*` | 输出路径、独立临时目录、提交和清理 |
| `src/platform/files.*` | Win32 句柄、路径规则、SHA-1/SHA-256/SHA-512、同目录句柄重命名 |
| `src/platform/jobs.*` | 本地任务摘要与打开结果 |
| `src/platform/log.*` | 原生 UTF-8 日志、步骤上下文、大小轮转、保留策略及 AppData 回退 |
| `src/platform/notifications.*` | AUMID、快捷方式、协议激活、原生 Toast |
| `tests/integration.ps1` | 生成无安装动作的 MSI，自制载荷与异常变体验证 |
| `tests/cab_burn_integration.py` | 已知内容 CAB/Burn、签名偏移、v3/v4 命名空间、外置/在线和损坏反例 |
| `tests/verify-burn.py` | 官方 Burn 样本、清单独立映射和载荷摘要、内层 MSI 与累计输出复核 |
| `tests/inno_integration.py` | 官方 ISCC 生成已知文件包，压缩/条件/版本与异常变体验证 |
| `tests/nsis_integration.py` | 官方 makensis 生成已知文件包，三种压缩/Solid/变量/冲突/CRC/异常变体 |
| `src/core/tree.*` | 内嵌 MSI/Inno/NSIS 展开、共享预算、去重、部分完成与报告 |
| `tests/nested_integration.py` | 官方 NSIS/Inno 惰性嵌套包、普通 EXE、不支持/损坏/动态路径、去重和深度/文件/字节/包数上限 |
| `tests/archive_integration.py` | 已知 ZIP/7z/SFX、ZIP64/描述符/Unicode、损坏与路径反例、NSIS 内嵌归档 |
| `tests/verify-installers.py` | 逐个检查指定目录安装包，逐文件摘要复核，并用官方 7zr 与独立 Inno 表读取核验载荷 |
| `tests/verify-nsis.py` | 用户 XnView 外壳逐字节对照，内层 1080 文件与独立读取的 Inno 位置表摘要比较 |
| `tests/path_tests.cpp` | 路径规则、目录共享锁、失败清理与短暂占用后的提交 |
| `tests/log_probe.cpp`、`tests/log_integration.py` | 独立日志探针、轮转与清理、权限回退、并发隔离、真实提取失败诊断 |
| `tests/notification_activation.cpp` | 手动协议冷启动探针 |
| `tests/verify-putty.ps1` | 真实 PuTTY 样本和官方摘要对照 |
| `tests/verify-inno.ps1` | 官方 Inno 安装包、已提取文件与生产方源码文件对照 |
| `tests/verify-textify.ps1` | 用户 Textify 样本与独立读取的包内 SHA-1 基准 |

自动测试使用系统 `makecab.exe` 和 Windows Installer COM 接口生成开发样本；它们不是产品的外部解包依赖。每轮使用独立目录，不执行安装器或输出文件。测试保留结果以便检查，生成物不进入 Git。

通知探针不纳入普通 CTest，因为它需要当前用户已注册协议。手动验证：

```powershell
cmake --build out/build/win-x64-release --target notification_activation_probe
Start-Process .\out\dist\win-x64-release\Extract.exe `
    -ArgumentList '--repair-notifications' -WindowStyle Hidden -Wait
.\out\build\win-x64-release\notification_activation_probe.exe
```

探针通过 Windows Shell 启动已退出的程序，核对固定帮助协议成功及非法任务 ID 拒绝。它不等于实际点击通知的视觉验收。

近期真实样本验证方法与结果、当前限制见 [MSI 实现与验证](06-msi-first-slice.md)和 [Inno 实现与验证](07-inno-implementation.md)。在干净 Windows 10/11、真实 Explorer 拖拽及通知横幅点击上的人工验收仍需补充。
