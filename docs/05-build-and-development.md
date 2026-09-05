# 构建与开发

更新日期：2026-09-05。适用于 0.1.0 工程骨架。

## 1. 技术栈与当前范围

- 自研主程序与未来的解析模块统一使用 C++20；允许链接 C 基础库，默认 C 标准为 C17。
- 原生 Win32，Windows 子系统，宽字符入口；初始构建目标为 Windows x64。
- CMake 3.25+、Ninja、MSVC；Visual Studio 预览版通过 `vswhere -prerelease` 自动发现。
- Release 使用静态 MSVC CRT；当前未引入第三方基础库。

本次只初始化工程，不包含格式识别、解包、输出目录管理、worker、系统通知或第三方库适配。没有完整解包能力，也没有对应的样本验收结果。

## 2. 推荐构建

在仓库根目录的 PowerShell 执行：

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release -Install
```

脚本自动发现包含 x64 C++ 工具链的 Visual Studio，调用 `vcvars64.bat`，然后运行对应 CMake 预设。工具链环境只写入当前进程，不修改用户或系统环境配置；优先使用 Visual Studio 自带的 CMake 和 Ninja。

如果本机脚本执行策略阻止运行，可在已初始化的 x64 Native Tools 终端直接执行下一节的 CMake 命令，无需修改全局执行策略。

## 3. 手动构建

以下命令要求当前终端已初始化 x64 MSVC 环境，并能找到 CMake 和 Ninja：

```powershell
cmake --preset win-x64-debug
cmake --build --preset win-x64-debug --parallel

cmake --preset win-x64-release
cmake --build --preset win-x64-release --parallel
cmake --install out/build/win-x64-release
```

| 路径 | 内容 |
| --- | --- |
| `out/build/win-x64-debug/bin/Extract.exe` | Debug 程序 |
| `out/build/win-x64-release/bin/Extract.exe` | Release 程序 |
| `out/dist/win-x64-release/` | 程序、README、开发文档和依赖说明 |
| `out/build/<预设>/compile_commands.json` | 供编辑器和静态分析使用的编译命令 |

`out/` 与本地预设 `CMakeUserPresets.json` 不进入 Git。不要在 Git 中提交真实安装包、编译产物、个人凭据或工具链缓存。

## 4. 初始化版本的行为

当前 EXE 仅提供启动基础，使用 Windows 自带的 `CommandLineToArgvW` 处理 Unicode 参数。路径中有空格时应按正常命令行规则加引号；多个路径可以作为多个参数传入。

| 输入 | 行为 | 退出码 |
| --- | --- | --- |
| `--help` | 输出当前阶段的使用说明 | 0 |
| `--version` | 输出版本及工程骨架标记 | 0 |
| 无参数、无效选项、仅 `--` | 输出参数说明 | 160（ERROR_BAD_ARGUMENTS） |
| 一个或多个路径，可在前面加 `--` | 报告尚未实现解包，不读取输入或生成文件 | 50（ERROR_NOT_SUPPORTED） |
| 未处理的内部异常 | 输出内部错误 | 574（ERROR_UNHANDLED_EXCEPTION） |

程序不创建控制台；命令行诊断复用父进程控制台或重定向句柄，并发送调试输出。右下角系统通知尚未实现，所以当前从 Explorer 拖入文件没有可见反馈。这是初始化版本的限制，不是最终交互方案。

Windows 子系统程序在部分终端中直接运行不会等待退出。可用以下命令检查版本并读取退出码：

```powershell
$process = Start-Process -FilePath .\out\build\win-x64-release\bin\Extract.exe `
    -ArgumentList '--version' -WindowStyle Hidden -Wait -PassThru
$process.ExitCode
```

## 5. 当前目录与后续开发

```text
CMakeLists.txt / CMakePresets.json   构建与安装
cmake/version.h.in                 由项目版本生成的头文件
src/app/main.cpp                   Win32 宽字符入口
src/platform/console.*             复用调用方输出，不创建窗口
resources/                         manifest 与文件版本资源
scripts/build.ps1                  工具链发现、配置、编译、安装
third_party/README.md              后续基础库的来源与许可规则
docs/                              产品规划、技术设计及开发说明
```

格式核心、通用输入/输出和测试目录在对应功能开始实现时再加入。后续按规划先完成近期格式研究和基础输入能力，再完成 MSI/CAB 的第一条解包链路。

初始化验证包含 Debug/Release 编译、启动参数和退出码烟雾检查、PE 子系统/清单/依赖检查。尚未建立格式测试集；加入实际解析逻辑时，应同步加入对应的边界、损坏输入和已知内容校验测试。

## 6. 本次初始化验证记录

2026-09-05 在本机 Windows 环境验证：

- Visual Studio 18 Insiders，MSVC 19.51.36256.0，CMake 4.3.1-msvc1；Debug/Release 均编译通过。
- 两种配置各验证 8 个启动场景，共 16 项通过：版本、帮助、无参数、未知选项、仅分隔符、中文空格路径、多路径、分隔符后的选项式文件名。
- 验证输入文件 SHA-256 未变化；便携目录中的 EXE 与 Release 构建产物 SHA-256 一致。
- Release 的 PE 信息为 x64、Windows GUI 子系统；包含 ASLR、DEP、CFG 标记。
- 内嵌清单为 `asInvoker` 和 `longPathAware=true`，文件/产品版本均为 0.1.0。
- Release 直接导入 `KERNEL32.dll` 和 `SHELL32.dll`，没有独立 MSVC CRT DLL 依赖。

以上属于工程构建和启动验证，未验证实际 Explorer 拖拽、系统通知、安装包解包或干净机器运行。
