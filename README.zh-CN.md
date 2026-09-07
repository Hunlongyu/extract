<p align="center">
  <img src="resources/logo.png" width="112" height="112" alt="Extract Logo">
</p>

<h1 align="center">Extract</h1>

<p align="center">拖入安装包，直接提取其中的文件。</p>
<p align="center"><strong>Windows 10 / 11 · x86 / x64 / ARM64 · 便携使用 · v0.6.0</strong></p>
<p align="center"><a href="README.md">English</a> | <strong>简体中文</strong></p>

---

Extract 是一款轻量的 Windows 安装包解包工具。没有复杂界面，将安装包拖到程序图标上即可开始，完成后通过系统通知查看结果。

适合希望直接获取应用文件、查看安装包内容，或尝试免安装使用软件的用户。Extract 只读取和提取文件，不运行安装程序。

## 功能特点

- **拖拽即用**：支持一次拖入多个安装包，无需逐个打开。
- **便携运行**：工具本身无需安装，不依赖额外的解包程序。
- **多种格式**：支持常见的 Inno Setup、NSIS、MSI、Burn，以及 ZIP、7z、CAB。
- **自动展开内层包**：识别安装包里嵌套的安装器，继续提取应用文件。
- **保留已有结果**：遇到同名输出目录自动加序号，支持中文和空格路径。
- **方便排查**：记录处理步骤、文件校验和失败原因，日志自动轮转与清理。

## 如何使用

1. 有正式版本后，从 [Releases](https://github.com/Hunlongyu/extract/releases) 下载适合系统架构的 EXE，放到本地磁盘，可重命名为 **`Extract.exe`**。本地构建也可使用便携目录中的 EXE。
2. 选中一个或多个安装包，拖到 **`Extract.exe` 的图标上**。
3. 等待系统通知。单个安装包成功时，点击通知打开结果目录；批量处理、部分完成或失败时，打开任务记录。

文件默认保存在**安装包旁边**，目录名为 `<安装包名称>_extracted`。已有同名目录时，自动使用 ` (2)`、` (3)` 等后缀。

Intel / AMD 64 位 Windows 选择 **x64**，ARM Windows 选择 **ARM64**，32 位 Windows 选择 **x86**。每个下载都是静态 CRT 的独立 EXE，无需安装 VC++ 运行库。发布同时提供校验和及第三方许可，转发程序时请保留许可文件。大型固实归档可能超过 x86 的可用地址空间。

```text
Downloads/
├─ Example-Setup.exe
└─ Example-Setup_extracted/
   ├─ app/                       应用文件（具体目录随格式而异）
   └─ _extract-report.json        文件清单与提取结果
```

如果安装包包含可识别的内层安装器，子结果保存在本层输出目录中，原内层包也会保留。部分 Electron / NSIS 包的应用文件位于 `app-64_extracted`，单个成功任务的通知会直接打开识别到的主要结果目录。

> 解包成功表示文件已按当前支持范围提取完成，不代表软件一定可以直接运行。依赖驱动、服务、注册表或额外运行库的软件，仍可能需要安装。

## 支持的安装包格式

| 格式 | 常见文件 | 当前支持范围 |
| --- | --- | --- |
| **Inno Setup** | `.exe` | 已验证官方 6.2.2、6.5.0、6.5.4、6.6.1、6.7.3、7.1.0 生成的内嵌包；支持无压缩、Deflate、bzip2、LZMA、LZMA2 |
| **NSIS** | `.exe` | Unicode 3.x 的已验证结构；覆盖官方 3.11 生成包及部分 3.12 结构样本，支持普通与固实压缩、部分 Electron / Tauri 封装 |
| **Windows Installer** | `.msi` | 内嵌或外置的单个、多 CAB，以及松散文件和混合源布局 |
| **WiX Burn** | `.exe` | 布局 2 的 CAB 容器、内嵌或本地外置载荷；覆盖 v3 / v4 清单结构，缺失在线载荷时报告部分完成 |
| **CAB** | `.cab` | 标准 Microsoft CAB 1.3；已验证无压缩、MSZIP、LZX |
| **ZIP / ZIP64** | `.zip` | 无压缩、Deflate、bzip2，支持中文路径与常见目录结构 |
| **7z** | `.7z` | Copy、LZMA、LZMA2，支持普通与固实块、BCJ / BCJ2 |
| **自解压封装（SFX）** | `.exe` | 可识别的 PE 附加区 ZIP、7z、CAB，直接读取其中的归档 |

`.exe` 是文件后缀，能否解包取决于其内部封装。上述范围按具体结构和样本验证，不代表同一安装器的所有版本、插件或定制封装均已支持。Inno 的精确数据版本与测试矩阵见 [兼容性说明](docs/07-inno-implementation.md)。

目前已实测提取 ECHO NEXT、vic-diary、Pebble、Textify 和 XnViewMP 的指定安装包样本，详见 [样本验证记录](docs/data/validation-0.6.0.json)。这些结果不等于对上述软件所有发行包的兼容承诺。

暂不支持 RAR、加密或多卷归档、跨卷 CAB、InstallShield 私有 CAB、MST / MSP、MSIX / APPX，以及需要联网下载才能取得的内容。MSI 使用外置 CAB 时，请将配套文件放在 MSI 同目录，松散文件按安装包的原始源目录布局提供。

## 结果与日志

| 提示 | 含义 |
| --- | --- |
| **文件提取完成** | 支持范围内的文件和已识别内层包已提取完成 |
| **部分完成** | 已保留提取出的文件，但仍有载荷缺失、目录未能还原，或内层包处理失败 |
| **失败** | 当前包未完成提取，任务记录中会说明原因 |

卸载器不属于必须还原的应用文件。已识别的卸载辅助文件即使原目录无法还原，也不会单独导致“部分完成”。

详细日志优先写入 **`Extract.exe` 同级的 `log` 目录**；不可读写时，自动回退至 **`%LOCALAPPDATA%\Extract\log`**。

- 默认总量 **10 MiB**，单文件 **2 MiB** 轮转，自动清理超过 **30 天**的旧日志。
- 日志记录输入包、内层包、处理阶段、文件校验、失败原因和系统错误码。
- 任务摘要保存在 `%LOCALAPPDATA%\Extract\jobs\`，其中的 `summary.txt` 标明本次详细日志位置。
- 日志可用记事本打开，不会自动上传。长任务超出限额后优先保留最近记录；活动或无法删除的文件可能使总量暂时超限。

通知被系统关闭或免打扰抑制时，可以使用下方的 `--open-last-result` 命令打开最近结果。更多说明见 [日志与排查](docs/15-logging.md)。

## 命令行用法

除拖拽外，也可以从 PowerShell 调用：

```powershell
# 指定输出父目录（该目录需要已经存在）
Start-Process .\Extract.exe -ArgumentList '--output "D:\Unpacked" "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait

# 静默提取：不发送通知，仍然记录日志
Start-Process .\Extract.exe -ArgumentList '--quiet "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait

# 打开最近一次任务的结果或记录
.\Extract.exe --open-last-result
```

| 参数 | 用途 |
| --- | --- |
| `--output <目录>` | 将结果写入指定的已有父目录 |
| `--quiet` | 关闭本次系统通知 |
| `--list <安装包>` | 将单个包的文件清单以 JSON 写入标准输出，不提取文件；仍记录过程日志 |
| `--open-last-result` | 打开最近结果或任务记录 |
| `--repair-notifications` | 移动便携目录后，更新通知注册路径 |
| `--unregister-notifications` | 移除当前用户的通知注册 |
| `--help` / `--version` | 查看帮助或版本 |

首次发送通知时会建立当前用户的通知注册。需要在脚本中获取退出码时，应等待程序结束；`0` 表示成功，`299` 表示部分完成或批次中存在成功与失败。

## 开发与更多说明

Extract 使用 **C++20 / C17、CMake 和 Win32** 开发。安装包解析与提取流程由本项目实现，底层压缩库静态链接。

- [构建与开发](docs/05-build-and-development.md)：工具链、编译命令与自动测试。
- [发布流程](docs/16-release.md)：仅版本标签触发 x86、x64、ARM64 构建，普通 push 不发布；ARM64 运行验证待完成。
- [产品需求](docs/02-requirements.md) · [技术设计](docs/03-technical-design.md)：行为说明、目录规划与资源限制。
- [开发计划](docs/04-roadmap-and-acceptance.md)：后续格式与功能范围。
- [第三方基础库与许可](third_party/README.md)：依赖版本、来源与署名。

当前仅处理本地普通文件，**不再设置统一的安装包大小、输出总量或文件总数上限**。提取会检查实际磁盘空间及整数范围，仍受格式、解码器、内存和地址空间约束；保留元数据和解析工作量保护。自动递归最多 **32 个包、4 层**，并检测嵌套循环。详见[大安装包与资源处理](docs/17-large-packages.md)。
