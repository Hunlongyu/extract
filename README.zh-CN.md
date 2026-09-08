<p align="center">
  <img src="resources/logo.png" width="112" height="112" alt="Extract Logo">
</p>

<h1 align="center">Extract</h1>

<p align="center">拖入安装包，直接提取其中的文件。</p>
<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4?style=flat-square" alt="Windows 10 / 11">
  <img src="https://img.shields.io/badge/Arch-x86%20%7C%20x64%20%7C%20ARM64-64748B?style=flat-square" alt="架构：x86、x64、ARM64">
  <img src="https://img.shields.io/badge/Portable-Single%20EXE-16A34A?style=flat-square" alt="便携运行：单个 EXE">
  <img src="https://img.shields.io/badge/Version-v0.8.0-7C3AED?style=flat-square" alt="版本 v0.8.0">
</p>
<p align="center"><a href="README.md">English</a> | <strong>简体中文</strong></p>

<p align="center">
  <a href="https://github.com/Hunlongyu/extract/releases">下载程序</a> ·
  <a href="#如何使用">快速上手</a> ·
  <a href="#支持的安装包格式">支持格式</a>
</p>

---

Extract 是一款轻量的 Windows 安装包解包工具。将一个或多个安装包拖到程序图标上，即可提取文件，并通过系统通知查看结果。没有主界面，无需安装。

## 功能特点

| 📦 单文件运行 | 🛡️ 静态解包 |
| :--- | :--- |
| 不依赖额外的解包程序，无需安装 VC++ 运行库。 | 只读取和提取文件，不运行安装程序。 |
| **🪆 自动展开内层包** | **🌐 Unicode 路径** |
| 识别安装包里嵌套的安装器，继续提取应用文件。 | 支持中文、其他 Unicode 字符和空格路径。 |

## 如何使用

1. 从 [Releases](https://github.com/Hunlongyu/extract/releases) 下载 EXE 并放到本地磁盘。Intel / AMD 64 位 Windows 选择 **x64**，ARM Windows 选择 **ARM64**，32 位 Windows 选择 **x86**。可重命名为 `Extract.exe`。
2. 选中一个或多个安装包，拖到 **`Extract.exe` 的图标上**。
3. 在结果通知中，单个安装包成功时点击 **打开文件夹**；批量处理、部分完成或失败时点击 **查看结果**。点击通知正文也可打开同一位置。

文件默认保存在**安装包旁边**，目录名为 `<安装包名称>_extracted`。已有同名目录时，自动添加 ` (2)`、` (3)` 等后缀，保留已有结果。内层包与其提取出的子目录也会一并保留。

耗时任务会显示当前包与处理阶段。进度条按当前包已写入的文件字节计数；准备等阶段显示活动状态和已处理字节，不估算百分比。内层包单独计数。结束后，同一条通知显示包名与结果，不再另弹完成提醒；短任务只显示结果。横幅收起后，可在通知中心继续查看。点击进度通知中的 **取消任务**，可停止当前包及批次中剩余的包，已完成的输出会保留。

**部分完成**表示已保留提取出的文件，但仍有内容缺失、目录未能还原，或内层包处理失败，具体原因可查看任务记录。没有收到通知时，可运行 `Extract.exe --open-last-result` 打开最近结果。

> 依赖驱动、服务、注册表或额外运行库的应用，解包后仍可能需要安装才能运行。

## 支持的安装包格式

| 格式 | 常见文件 | 当前支持范围 |
| :--- | :--- | :--- |
| **Inno Setup** | `.exe`、配套 `.bin` | 支持 6.0–6.7 和 7.1 的部分标准布局及[外置分卷](docs/26-split-volumes.md)，详见[兼容性说明](docs/20-inno-compatibility.md) |
| **NSIS** | `.exe` | 已验证的 Unicode 3.x 与部分 [ANSI 2.x 布局](docs/21-nsis-ansi-compatibility.md)、普通与固实压缩、部分 Electron / Tauri 封装；已识别的架构分支按目录分别保留 |
| **Windows Installer** | `.msi` | 内嵌、外置或混合 CAB 卷链及跨卷文件、松散文件及混合源布局 |
| **WiX Burn** | `.exe` | 布局 2 的 CAB 容器，支持 v3 / v4 清单及内嵌或本地外置载荷 |
| **Velopack / Squirrel.Windows** | `.exe`、`*-full.nupkg` | [已验证的离线布局](docs/24-update-packages.md)，应用文件集中到 `app`；多个目标分别保留目录 |
| **MSIX / APPX / Bundle** | `.msix`、`.appx`、`.msixbundle`、`.appxbundle` | [未加密离线包](docs/25-msix-appx.md)，校验块哈希，内嵌架构与资源包分目录展开 |
| **CAB** | `.cab` | 标准 Microsoft CAB 1.3：无压缩、MSZIP、LZX，支持跨卷续接 |
| **ZIP / ZIP64** | `.zip` | 无压缩、Deflate、bzip2 |
| **7z** | `.7z` | Copy、LZMA、LZMA2，支持普通与固实块、BCJ / BCJ2 |
| **自解压封装（SFX）** | `.exe` | 可识别的 PE 附加区 ZIP、7z、CAB 归档 |

能否解包取决于内部格式，而非 `.exe` 后缀。定制封装和未验证的版本可能不受支持。差分更新包缺少基础版本时无法还原。

MSIX / APPX 解包保留清单、资源和原始 `VFS` 目录，不执行部署或恢复安装身份，提取的应用仍可能需要安装。Bundle 保留全部内嵌架构和资源包；缺少外置成员时显示部分完成。

分卷包请将所有卷放在同一目录，保留原始文件名。拖入 Inno 的 EXE、MSI，或标准 CAB 卷链中的任意一卷即可；缺卷时会提示具体文件名。MSI 松散文件需保留原始源目录布局。

暂不支持 RAR、加密包、分卷 ZIP / 7z、InstallShield 私有 CAB、MST / MSP，以及需要联网下载才能取得的内容。Burn 内部 CAB 容器的跨卷续接尚未支持。

当前仅处理本地普通文件，不设置统一的安装包大小、输出总量或文件总数上限，但仍受磁盘空间、内存和格式限制。处理大包时，优先选择 x64 或 ARM64，详见[资源处理说明](docs/17-large-packages.md)。

## 命令行用法

在 PowerShell 中指定已有的输出父目录：

```powershell
Start-Process .\Extract.exe -ArgumentList '--output "D:\Unpacked" "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait
```

使用 `--quiet` 关闭通知，或用 `--list <安装包>` 输出 JSON 文件清单而不提取文件。更多参数和退出码见[完整命令行说明](docs/05-build-and-development.md#使用与退出码)。

## 开发

使用 **C++20 / C17、CMake 和 Win32** 开发，安装包解析由本项目实现，底层压缩库静态链接。

v0.8.0 新增 Velopack / Squirrel、MSIX / APPX、Inno / CAB 分卷支持和任务取消，详见[版本说明](docs/releases/v0.8.0.md)。

- [构建与开发](docs/05-build-and-development.md)
- [发布流程](docs/16-release.md)：仅版本标签触发构建，普通 push 不发布；ARM64 运行验证待完成。
- [开发计划](docs/04-roadmap-and-acceptance.md)
- [工作进程与取消](docs/22-worker-cancellation.md)：取消任务、可选超时及工作进程异常恢复。
- [第三方基础库与许可](third_party/README.md)：转发程序时请保留许可文件。
