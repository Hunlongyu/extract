<p align="center">
  <img src="resources/logo.png" width="112" height="112" alt="Extract Logo">
</p>

<h1 align="center">Extract</h1>

<p align="center">Drop an installer. Get the files inside.</p>
<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4?style=flat-square" alt="Windows 10 / 11">
  <img src="https://img.shields.io/badge/Arch-x86%20%7C%20x64%20%7C%20ARM64-64748B?style=flat-square" alt="Architectures: x86, x64, ARM64">
  <img src="https://img.shields.io/badge/Portable-Single%20EXE-16A34A?style=flat-square" alt="Portable: single EXE">
  <a href="https://github.com/Hunlongyu/extract/releases/tag/v0.9.0"><img src="https://img.shields.io/badge/Version-v0.9.0-7C3AED?style=flat-square" alt="Version v0.9.0"></a>
  <a href="https://github.com/Hunlongyu/extract/releases"><img src="https://img.shields.io/github/downloads/Hunlongyu/extract/total?style=flat-square&amp;color=16A34A" alt="Total downloads of all release assets"></a>
</p>
<p align="center"><strong>English</strong> | <a href="README.zh-CN.md">简体中文</a></p>

<p align="center">
  <a href="#download">Download</a> ·
  <a href="#getting-started">Quick start</a> ·
  <a href="#supported-formats">Supported formats</a> ·
  <a href="docs/releases/v0.9.0.md">What's new</a>
</p>

---

Extract is a lightweight, zero-configuration Windows installer unpacker. Simply drag and drop one or more packages onto the executable to extract their files—no installation or GUI required.

## Download

Standalone portable executable. No external extraction tools or VC++ runtime installation required:

| Architecture | Platform | Download v0.9.0 |
| :--- | :--- | :--- |
| **x64** | Most 64-bit PCs (Recommended) | [Download x64](https://github.com/Hunlongyu/extract/releases/download/v0.9.0/Extract-v0.9.0-windows-x64.exe) |
| **ARM64** | Windows on ARM devices | [Download ARM64](https://github.com/Hunlongyu/extract/releases/download/v0.9.0/Extract-v0.9.0-windows-arm64.exe) |
| **x86** | 32-bit Windows | [Download x86](https://github.com/Hunlongyu/extract/releases/download/v0.9.0/Extract-v0.9.0-windows-x86.exe) |

[Release notes](docs/releases/v0.9.0.md) · [SHA-256 checksums](https://github.com/Hunlongyu/extract/releases/download/v0.9.0/SHA256SUMS.txt) · [All releases](https://github.com/Hunlongyu/extract/releases)

## Features

- **Drag & drop** — unpack single or batched installers with native Windows notification updates.
- **Pure static extraction** — safe parsing without running any installer, embedded script, or unpacked executable.
- **Nested archive support** — automatically unpacks recognized inner installers and archives recursively.
- **Smart compact layout** — elevates main application files to the output root while neatly organizing extra files and architectures.
- **Full Unicode support** — handles Chinese, special characters, spaces, and long paths seamlessly.

## Getting started

1. [Download](#download) the executable for your architecture (optionally rename it to `Extract.exe`).
2. Drag and drop one or more installer packages **onto the `Extract.exe` icon**.
3. Once completed, click **Open folder** (or **View results**) on the notification to view the extracted files.

Files are extracted next to the installer under `<package_name>_extracted`. Existing folders are protected by appending ` (2)`, ` (3)`, etc.

### Notes

- **Output layout**: Uses **compact layout** by default, placing the primary application at the root and extra files in `_extra`. To preserve raw logical paths, pass `--layout original` (see [layout rules](docs/28-compact-layout.md)).
- **Progress & cancellation**: Long-running tasks show a progress notification with a cancel option (partial output is kept). If no notification appears, run `Extract.exe --open-last-result`.
- **Scripts & analysis**: Decompiled installation logic (such as NSIS scripts) is exported as static text to `_extract-script/nsis-script.txt` for inspection, never executed (see [NSIS notes](docs/27-nsis-shell-directories.md)).

> [!NOTE]
> Extract retrieves files only. If the application depends on device drivers, services, or registry keys, it may still require standard installation to run.

## Supported formats

| Format | Extension | Support Details |
| :--- | :--- | :--- |
| **Inno Setup** | `.exe`, `.bin` | Inno Setup 6.0–6.7 & 7.1 standard layouts and [external volumes](docs/26-split-volumes.md) ([details](docs/20-inno-compatibility.md)) |
| **NSIS** | `.exe` | Unicode 3.x and selected [ANSI 2.x](docs/21-nsis-ansi-compatibility.md), solid compression, Electron / Tauri wrappers, multi-architecture splitting |
| **Windows Installer** | `.msi` | Embedded, external, or mixed CAB sets, spanning files, loose files, and mixed source trees |
| **WiX Burn** | `.exe` | Layout 2 CAB containers with v3 / v4 manifests, embedded or local external payloads |
| **Velopack / Squirrel** | `.exe`, `*.nupkg` | [Offline update packages](docs/24-update-packages.md); automatically groups application files |
| **MSIX / APPX / Bundle** | `.msix`, `.appx`, `.msixbundle`... | [Unencrypted offline packages](docs/25-msix-appx.md), block hash verification, architecture & resource splitting |
| **CAB** | `.cab` | Microsoft CAB 1.3: stored, MSZIP, LZX, including multi-part spanning sets |
| **ZIP / ZIP64** | `.zip` | Stored, Deflate, and bzip2 |
| **7z** | `.7z` | LZMA, LZMA2, Copy; solid blocks and BCJ / BCJ2 filters |
| **Self-extracting (SFX)** | `.exe` | Recognized ZIP, 7z, and CAB payloads embedded in PE overlays |

### Notes & Limitations

- **Split volumes**: Place all volumes in the same folder with original filenames. Drag the main Inno EXE, MSI, or any CAB volume in a set.
- **Large packages**: No arbitrary size or file-count caps; files are processed based on available system resources. 64-bit builds are recommended for massive files (see [resource handling](docs/17-large-packages.md)).
- **Unsupported**: Encrypted archives, RAR, split ZIP/7z archives, InstallShield proprietary CABs, MST/MSP patches, and web/online download stubs.

## Command-line usage

In addition to drag-and-drop, you can run Extract from scripts or the command line:

```powershell
# Extract to a custom output directory
Start-Process .\Extract.exe -ArgumentList '--output "D:\Unpacked" "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait
```

| Option | Purpose |
| :--- | :--- |
| `--output <path>` | Specify a destination parent directory |
| `--layout compact` | Use the default smart compact layout |
| `--layout original` | Preserve original logical folders without compact reorganizing |
| `--list <package>` | List logical paths within the package without extracting |
| `--quiet` | Run silently without desktop notifications |
| `--open-last-result` | Open the output folder of the most recent task |

For complete options and exit codes, see the [command-line reference](docs/05-build-and-development.md#使用与退出码).

## Development

Built with **C++20 / C17, CMake, and Win32 APIs**, featuring self-contained parsers and statically linked compression libraries.

- [Build and development](docs/05-build-and-development.md)
- [Release notes](docs/releases/v0.9.0.md)
- [Roadmap](docs/04-roadmap-and-acceptance.md)
- [Worker isolation & cancellation](docs/22-worker-cancellation.md)
- [Third-party libraries & licenses](third_party/README.md)
