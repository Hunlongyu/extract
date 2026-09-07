<p align="center">
  <img src="resources/logo.png" width="112" height="112" alt="Extract Logo">
</p>

<h1 align="center">Extract</h1>

<p align="center">Drop an installer. Get the files inside.</p>
<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4?style=flat-square" alt="Windows 10 / 11">
  <img src="https://img.shields.io/badge/Arch-x86%20%7C%20x64%20%7C%20ARM64-64748B?style=flat-square" alt="Architectures: x86, x64, ARM64">
  <img src="https://img.shields.io/badge/Portable-Single%20EXE-16A34A?style=flat-square" alt="Portable: single EXE">
  <img src="https://img.shields.io/badge/Version-v0.7.0-7C3AED?style=flat-square" alt="Version v0.7.0">
</p>
<p align="center"><strong>English</strong> | <a href="README.zh-CN.md">简体中文</a></p>

<p align="center">
  <a href="https://github.com/Hunlongyu/extract/releases">Downloads</a> ·
  <a href="#getting-started">Quick start</a> ·
  <a href="#supported-formats">Supported formats</a>
</p>

---

Extract is a lightweight Windows installer extractor. Drop one or more packages onto the executable to retrieve their files, then view the results through a system notification. No main window or installation is required.

## Features

| 📦 Standalone EXE | 🛡️ Static extraction |
| :--- | :--- |
| No external extraction tools or VC++ runtime installation required. | Inspect and extract files without running the installer. |
| **🪆 Nested extraction** | **🌐 Unicode paths** |
| Automatically unpack recognized installers inside a package. | Supports names containing Unicode characters and spaces. |

## Getting started

1. Download the EXE from [Releases](https://github.com/Hunlongyu/extract/releases) and save it on a local drive. Choose **x64** for Intel / AMD 64-bit Windows, **ARM64** for ARM Windows, or **x86** for 32-bit Windows. You may rename it to `Extract.exe`.
2. Select one or more installers and **drop them onto the `Extract.exe` icon**.
3. Use **Open folder** in the result notification for a single successful extraction, or **View results** for batches, partial results, and failures. Clicking the notification itself opens the same destination. Button labels currently appear in Chinese.

Files are saved **next to the input package** in `<package-name>_extracted`. Existing folders are preserved by adding ` (2)`, ` (3)`, and so on. Nested packages are retained alongside their extracted subfolders.

Longer tasks show the current package and stage. The progress bar tracks file bytes written for the current package; preparation and other stages show activity and processed bytes instead of an estimated percentage. Nested packages have their own progress. The same notification then shows the result and package name without another popup; short tasks show only the result. Check Notification Center if the banner closes.

A **partial result** means extracted files were retained, but some content is missing, paths could not be restored, or a nested package failed. The job record explains the issue. If no notification appears, run `Extract.exe --open-last-result` to open the latest result.

> Extracted applications may still require installation if they depend on drivers, services, registry settings, or additional runtimes.

## Supported formats

| Format | Typical files | Current support |
| :--- | :--- | :--- |
| **Inno Setup** | `.exe` | Selected standard layouts from 6.0–6.7 and 7.1; see [compatibility notes](docs/20-inno-compatibility.md) |
| **NSIS** | `.exe` | Tested Unicode 3.x and selected [ANSI 2.x layouts](docs/21-nsis-ansi-compatibility.md), ordinary and solid compression, selected Electron / Tauri wrappers, and separate folders for recognized architecture branches |
| **Windows Installer** | `.msi` | Embedded or external CABs, loose files, and mixed source layouts |
| **WiX Burn** | `.exe` | Layout 2 CAB containers with v3 / v4 manifests; embedded or local external payloads |
| **CAB** | `.cab` | Standard Microsoft CAB 1.3: stored, MSZIP, and LZX |
| **ZIP / ZIP64** | `.zip` | Stored, Deflate, and bzip2 |
| **7z** | `.7z` | Copy, LZMA, and LZMA2; ordinary and solid blocks, BCJ / BCJ2 |
| **Self-extracting wrappers (SFX)** | `.exe` | Recognized ZIP, 7z, and CAB archives in PE overlays |

Compatibility depends on the package's internal format, not its `.exe` extension. Customized packages and untested versions may be unsupported.

Currently unsupported: RAR, encrypted or multi-volume archives, spanning CABs, InstallShield private CABs, MST / MSP, MSIX / APPX, and content that must be downloaded. For MSI packages, keep external CABs beside the MSI and preserve the original source directory layout for loose files.

Only regular files on local drives are supported. There is no fixed package-size, output-size, or file-count ceiling, but available disk space, memory, and format limits still apply. Prefer x64 or ARM64 over x86 for large packages; see [resource handling](docs/17-large-packages.md) for details.

## Command-line usage

To choose an existing output parent folder in PowerShell:

```powershell
Start-Process .\Extract.exe -ArgumentList '--output "D:\Unpacked" "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait
```

Use `--quiet` to suppress notifications or `--list <package>` to output a JSON file list without extracting files. See the [full command-line reference](docs/05-build-and-development.md#使用与退出码) for more options and exit codes.

## Development

Built with **C++20 / C17, CMake, and Win32**, with project-owned package parsers and statically linked compression libraries. Detailed developer documents are currently in Chinese.

- [Build and development](docs/05-build-and-development.md)
- [Release workflow](docs/16-release.md) — version tags trigger builds; ordinary pushes do not publish. ARM64 runtime testing is pending.
- [Roadmap](docs/04-roadmap-and-acceptance.md)
- [Third-party libraries and licenses](third_party/README.md) — retain the notices when redistributing the program.
