<p align="center">
  <img src="resources/logo.png" width="112" height="112" alt="Extract Logo">
</p>

<h1 align="center">Extract</h1>

<p align="center">拖入安装包，直接提取文件。</p>
<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4?style=flat-square" alt="Windows 10 / 11">
  <img src="https://img.shields.io/badge/Arch-x86%20%7C%20x64%20%7C%20ARM64-64748B?style=flat-square" alt="架构：x86、x64、ARM64">
  <img src="https://img.shields.io/badge/Portable-Single%20EXE-16A34A?style=flat-square" alt="便携运行：单个 EXE">
  <a href="https://github.com/Hunlongyu/extract/releases/tag/v0.9.0"><img src="https://img.shields.io/badge/Version-v0.9.0-7C3AED?style=flat-square" alt="版本 v0.9.0"></a>
  <a href="https://github.com/Hunlongyu/extract/releases"><img src="https://img.shields.io/github/downloads/Hunlongyu/extract/total?style=flat-square&amp;color=16A34A" alt="累计下载次数（全部发布附件）"></a>
</p>
<p align="center"><a href="README.md">English</a> | <strong>简体中文</strong></p>

<p align="center">
  <a href="#下载">下载程序</a> ·
  <a href="#如何使用">快速上手</a> ·
  <a href="#支持的格式">支持格式</a> ·
  <a href="docs/releases/v0.9.0.md">版本更新</a>
</p>

---

Extract 是一款轻量、免安装的 Windows 安装包解包工具。只需将一个或多个安装包拖到程序图标上，即可自动提取全部文件并通过系统通知展示结果，无需主界面与繁琐配置。

## 下载

单文件绿色版，无需安装，无外部依赖或运行库要求：

| 架构 | 适用平台 | 下载 v0.9.0 |
| :--- | :--- | :--- |
| **x64** | 大多数 64 位 PC（推荐） | [下载 x64](https://github.com/Hunlongyu/extract/releases/download/v0.9.0/Extract-v0.9.0-windows-x64.exe) |
| **ARM64** | Windows on ARM 设备 | [下载 ARM64](https://github.com/Hunlongyu/extract/releases/download/v0.9.0/Extract-v0.9.0-windows-arm64.exe) |
| **x86** | 32 位 Windows | [下载 x86](https://github.com/Hunlongyu/extract/releases/download/v0.9.0/Extract-v0.9.0-windows-x86.exe) |

[版本说明](docs/releases/v0.9.0.md) · [SHA-256 校验和](https://github.com/Hunlongyu/extract/releases/download/v0.9.0/SHA256SUMS.txt) · [全部版本](https://github.com/Hunlongyu/extract/releases)

## 功能特点

- **拖拽即解**：支持单个或批量安装包拖入，通过原生系统通知实时反馈进度和结果。
- **安全纯静态**：纯解析读取，绝不执行安装器本身、内嵌脚本或释放出的程序。
- **智能嵌套解包**：自动识别并递归解包嵌套在内的其他安装包与归档。
- **精简目录布局**：自动提取核心程序至根目录，附加组件与架构分支有序归类。
- **完善路径支持**：原生支持中文、空格、特殊 Unicode 字符及长路径。

## 如何使用

1. [下载](#下载)对应架构的 EXE（可重命名为 `Extract.exe`）放到本地。
2. 选中一个或多个安装包，**拖拽到 `Extract.exe` 图标上**。
3. 提取完成后，点击通知上的 **打开文件夹**（或 **查看结果**）即可浏览提取出的文件。

默认解压到安装包同级目录下的 `<安装包名>_extracted`。遇到同名文件夹会自动递增后缀（如 ` (2)`），保护已有数据。

### 使用说明

- **目录布局**：默认采用**精简布局**，核心主程序直接置于根目录，附属文件归入 `_extra`；如需完整还原原始逻辑层级，可使用 `--layout original`（详见[布局规则](docs/28-compact-layout.md)）。
- **进度与取消**：耗时任务会实时更新进度条并支持中途取消（已解压文件会保留）；若未弹出通知，可运行 `Extract.exe --open-last-result` 打开最近结果。
- **脚本与分析**：NSIS 等安装程序的编译脚本与指令会导出为 `_extract-script/nsis-script.txt` 静态文本，仅作逆向分析记录，不执行任何代码（详见 [NSIS 说明](docs/27-nsis-shell-directories.md)）。

> [!NOTE]
> 解包工具仅提取程序文件。若目标软件依赖系统驱动、服务或注册表项，提取后可能仍需正常安装才能使用。

## 支持的格式

| 格式 | 扩展名 | 支持范围 |
| :--- | :--- | :--- |
| **Inno Setup** | `.exe`, `.bin` | 支持 6.0–6.7、7.1 标准布局与[外置分卷](docs/26-split-volumes.md)（[详情](docs/20-inno-compatibility.md)） |
| **NSIS** | `.exe` | Unicode 3.x 与部分 [ANSI 2.x](docs/21-nsis-ansi-compatibility.md)，普通/固实压缩，Electron/Tauri 封装与架构分支识别 |
| **Windows Installer** | `.msi` | 内嵌/外置/混合 CAB 卷链、跨卷文件及松散文件源布局 |
| **WiX Burn** | `.exe` | 布局 2 CAB 容器，支持 v3/v4 清单及内嵌/本地载荷 |
| **Velopack / Squirrel** | `.exe`, `*.nupkg` | [离线更新包](docs/24-update-packages.md)，自动归集应用本体与分架构目录 |
| **MSIX / APPX / Bundle** | `.msix`, `.appx`, `.msixbundle`... | [未加密离线包](docs/25-msix-appx.md)，块哈希校验，分架构与资源包展开 |
| **CAB** | `.cab` | 标准 Microsoft CAB 1.3：无压缩、MSZIP、LZX，支持跨卷续接 |
| **ZIP / ZIP64** | `.zip` | 存储（Stored）、Deflate 与 bzip2 |
| **7z** | `.7z` | LZMA、LZMA2、Copy，支持固实块与 BCJ/BCJ2 过滤器 |
| **自解压封装 (SFX)** | `.exe` | 识别 PE 附加区内的 ZIP、7z 及 CAB 归档 |

### 说明与限制

- **分卷安装包**：请将所有分卷置于同一目录并保持原名，直接拖入主 EXE、MSI 或任一 CAB 卷即可。
- **大包支持**：无统一包体积或文件数量上限，按实际系统资源流式处理；处理超大安装包建议优先使用 64 位版本（详见[资源处理说明](docs/17-large-packages.md)）。
- **暂不支持**：RAR 格式、密码加密、分卷 ZIP/7z、InstallShield 私有 CAB、MST/MSP 补丁以及需联网下载的在线安装器。

## 命令行用法

除拖拽外，也可通过命令行静默调用或集成到脚本中：

```powershell
# 指定输出目录解包
Start-Process .\Extract.exe -ArgumentList '--output "D:\Unpacked" "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait
```

| 参数 | 说明 |
| :--- | :--- |
| `--output <目录>` | 指定提取结果的目标父目录 |
| `--layout compact` | 使用默认的智能精简布局 |
| `--layout original` | 保留原始逻辑层级，跳过精简布局整理 |
| `--list <安装包>` | 仅列出包内文件路径，不执行提取 |
| `--quiet` | 静默模式，不弹出系统通知 |
| `--open-last-result` | 打开最近一次的解包输出目录 |

完整参数与退出码说明见[开发与命令行文档](docs/05-build-and-development.md#使用与退出码)。

## 开发

基于 **C++20 / C17、CMake 与 Win32 API**，自主实现格式解析，底层压缩库静态链接（无外部运行库依赖）。

- [构建与开发指南](docs/05-build-and-development.md)
- [版本发布说明](docs/releases/v0.9.0.md)
- [项目路线图](docs/04-roadmap-and-acceptance.md)
- [工作进程与异常隔离机制](docs/22-worker-cancellation.md)
- [第三方库与开源许可](third_party/README.md)
