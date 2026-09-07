<p align="center">
  <img src="resources/logo.png" width="112" height="112" alt="Extract Logo">
</p>

<h1 align="center">Extract</h1>

<p align="center">拖入安装包，直接提取其中的文件。</p>
<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4?style=flat-square" alt="Windows 10 / 11">
  <img src="https://img.shields.io/badge/Arch-x86%20%7C%20x64%20%7C%20ARM64-64748B?style=flat-square" alt="架构：x86、x64、ARM64">
  <img src="https://img.shields.io/badge/Portable-Single%20EXE-16A34A?style=flat-square" alt="便携运行：单个 EXE">
  <img src="https://img.shields.io/badge/Version-v0.6.1-7C3AED?style=flat-square" alt="版本 v0.6.1">
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
3. 等待系统通知。单个安装包成功时，点击通知打开结果目录；批量处理、部分完成或失败时，打开任务记录。

文件默认保存在**安装包旁边**，目录名为 `<安装包名称>_extracted`。已有同名目录时，自动添加 ` (2)`、` (3)` 等后缀，保留已有结果。内层包与其提取出的子目录也会一并保留。

耗时任务会显示当前包、处理阶段及实际字节进度，内层包单独计数。结束后，同一条通知显示包名与结果，不再另弹完成提醒；短任务只显示结果。横幅收起后，可在通知中心继续查看。

**部分完成**表示已保留提取出的文件，但仍有内容缺失、目录未能还原，或内层包处理失败，具体原因可查看任务记录。没有收到通知时，可运行 `Extract.exe --open-last-result` 打开最近结果。

> 依赖驱动、服务、注册表或额外运行库的应用，解包后仍可能需要安装才能运行。

## 支持的安装包格式

| 格式 | 常见文件 | 当前支持范围 |
| :--- | :--- | :--- |
| **Inno Setup** | `.exe` | 已验证部分 6.x / 7.x 版本的内嵌包，详见[兼容性说明](docs/07-inno-implementation.md) |
| **NSIS** | `.exe` | 已验证的 Unicode 3.x 结构、普通与固实压缩，以及部分 Electron / Tauri 封装 |
| **Windows Installer** | `.msi` | 内嵌或外置 CAB、松散文件及混合源布局 |
| **WiX Burn** | `.exe` | 布局 2 的 CAB 容器，支持 v3 / v4 清单及内嵌或本地外置载荷 |
| **CAB** | `.cab` | 标准 Microsoft CAB 1.3：无压缩、MSZIP、LZX |
| **ZIP / ZIP64** | `.zip` | 无压缩、Deflate、bzip2 |
| **7z** | `.7z` | Copy、LZMA、LZMA2，支持普通与固实块、BCJ / BCJ2 |
| **自解压封装（SFX）** | `.exe` | 可识别的 PE 附加区 ZIP、7z、CAB 归档 |

能否解包取决于内部格式，而非 `.exe` 后缀。定制封装和未验证的版本可能不受支持。

暂不支持 RAR、加密或多卷归档、跨卷 CAB、InstallShield 私有 CAB、MST / MSP、MSIX / APPX，以及需要联网下载才能取得的内容。MSI 的外置 CAB 请放在 MSI 同目录，松散文件需保留原始源目录布局。

当前仅处理本地普通文件，不设置统一的安装包大小、输出总量或文件总数上限，但仍受磁盘空间、内存和格式限制。处理大包时，优先选择 x64 或 ARM64，详见[资源处理说明](docs/17-large-packages.md)。

## 命令行用法

在 PowerShell 中指定已有的输出父目录：

```powershell
Start-Process .\Extract.exe -ArgumentList '--output "D:\Unpacked" "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait
```

使用 `--quiet` 关闭通知，或用 `--list <安装包>` 输出 JSON 文件清单而不提取文件。更多参数和退出码见[完整命令行说明](docs/05-build-and-development.md#使用与退出码)。

## 开发

使用 **C++20 / C17、CMake 和 Win32** 开发，安装包解析由本项目实现，底层压缩库静态链接。

- [构建与开发](docs/05-build-and-development.md)
- [发布流程](docs/16-release.md)：仅版本标签触发构建，普通 push 不发布；ARM64 运行验证待完成。
- [开发计划](docs/04-roadmap-and-acceptance.md)
- [第三方基础库与许可](third_party/README.md)：转发程序时请保留许可文件。
