<p align="center">
  <img src="resources/logo.png" width="112" height="112" alt="Extract Logo">
</p>

<h1 align="center">Extract</h1>

<p align="center">Drop an installer. Get the files inside.</p>
<p align="center"><strong>Windows 10 / 11 · x86 / x64 / ARM64 · Portable · v0.6.0</strong></p>
<p align="center"><strong>English</strong> | <a href="README.zh-CN.md">简体中文</a></p>

---

Extract is a lightweight Windows tool for unpacking installers. Drop one or more packages onto the executable, then use the system notification to find the results. There is no main application window to navigate.

Use it to inspect package contents, retrieve application files, or try running an application without installing it. Extract reads and extracts files without running the installer.

## Features

- **Drag and drop** — process one installer or a batch of files.
- **Portable** — no installation or external extraction tools required.
- **Multiple formats** — Inno Setup, NSIS, MSI, Burn, ZIP, 7z, and CAB.
- **Nested extraction** — automatically unpack recognized installers inside a package.
- **Preserve existing results** — add a numbered suffix when an output folder already exists; Unicode paths and spaces are supported.
- **Useful diagnostics** — record extraction steps, file verification, and failure details, with automatic log rotation and cleanup.

## Getting started

1. Download the EXE for your Windows architecture from [Releases](https://github.com/Hunlongyu/extract/releases), when a release is available. Save it on a local drive; you may rename it to **`Extract.exe`**. For a local build, use the EXE from the portable folder.
2. Select one or more installers and **drop them onto the `Extract.exe` icon**.
3. Wait for the system notification. For a single successful extraction, clicking it opens the result folder. For batches, partial results, or failures, it opens the job record.

Files are saved **next to the input package** in `<package-name>_extracted`. If the folder already exists, Extract adds ` (2)`, ` (3)`, and so on.

Choose **x64** for Intel / AMD 64-bit Windows, **ARM64** for ARM Windows, or **x86** for 32-bit Windows. Each download is a standalone EXE with a static CRT; no VC++ runtime installation is required. Releases also include checksums and third-party notices. Keep the notices when redistributing the program. Large solid archives may exceed x86's available address space.

```text
Downloads/
├─ Example-Setup.exe
└─ Example-Setup_extracted/
   ├─ app/                       Application files; layout varies by format
   └─ _extract-report.json        File list and extraction results
```

Recognized nested installers are extracted into subfolders, and the original nested packages are retained. Some Electron / NSIS packages place their application files in `app-64_extracted`. For a single successful task, the notification opens the identified primary result folder.

> Successful extraction does not guarantee that an application can run without installation. Software that depends on drivers, services, registry settings, or additional runtimes may still need to be installed.

## Supported formats

| Format | Typical files | Current support |
| --- | --- | --- |
| **Inno Setup** | `.exe` | Embedded packages built with official versions 6.2.2, 6.5.0, 6.5.4, 6.6.1, 6.7.3, and 7.1.0 have been tested; stored, Deflate, bzip2, LZMA, and LZMA2 |
| **NSIS** | `.exe` | Tested Unicode 3.x layouts, including official 3.11-generated packages and selected 3.12-layout samples; ordinary and solid compression, plus selected Electron / Tauri wrappers |
| **Windows Installer** | `.msi` | Single or multiple embedded/external CABs, loose files, and mixed source layouts |
| **WiX Burn** | `.exe` | Layout 2 with CAB containers and embedded or local external payloads; v3 / v4 manifest structures; missing online payloads produce a partial result |
| **CAB** | `.cab` | Standard Microsoft CAB 1.3; stored, MSZIP, and LZX have been tested |
| **ZIP / ZIP64** | `.zip` | Stored, Deflate, and bzip2, with Unicode paths and common directory structures |
| **7z** | `.7z` | Copy, LZMA, and LZMA2; ordinary and solid blocks, BCJ / BCJ2 |
| **Self-extracting wrappers (SFX)** | `.exe` | Recognized ZIP, 7z, and CAB archives in PE overlays, read directly without running the wrapper |

An `.exe` extension alone does not identify the installer format. Support is verified against specific layouts and samples, not every version, plugin, or customized package. See the [Inno compatibility notes](docs/07-inno-implementation.md) for exact data versions and the test matrix.

Specific ECHO NEXT, vic-diary, Pebble, Textify, and XnViewMP installers have passed extraction checks. See the [sample validation record](docs/data/validation-0.6.0.json). These results do not imply support for every release of those applications.

Currently unsupported: RAR, encrypted or multi-volume archives, spanning CABs, InstallShield private CABs, MST / MSP, MSIX / APPX, and content that must be downloaded. For MSI packages with external CABs, keep the companion files beside the MSI. Loose files must retain the package's original source directory layout.

## Results and logs

| Result | Meaning |
| --- | --- |
| **Completed** | Files within the supported scope and recognized nested packages were extracted |
| **Partially completed** | Extracted files were retained, but some payloads are missing, paths could not be restored, or a nested package failed |
| **Failed** | Extraction of the current package did not complete; the job record explains why |

Uninstallers are not required application payloads. An unresolved original path for a recognized uninstaller does not, by itself, cause a partial result.

Detailed logs go to **`log` beside `Extract.exe`**. If that location cannot be read or written, Extract falls back to **`%LOCALAPPDATA%\Extract\log`**.

- **10 MiB** total by default, with **2 MiB** per file and automatic cleanup of logs older than **30 days**.
- Records include the input package, nested packages, processing stages, file verification, failure reasons, and Windows error codes.
- Job summaries are stored under `%LOCALAPPDATA%\Extract\jobs\`. Each `summary.txt` identifies the detailed logs for that session.
- Logs can be opened in a text editor and are never uploaded automatically. Long runs retain the most recent records when the limit is reached; active or undeletable files may temporarily exceed the total limit.

If Windows disables or suppresses notifications, use `--open-last-result` to open the latest result. See [logging and troubleshooting](docs/15-logging.md) for details.

## Command-line usage

You can also invoke Extract from PowerShell:

```powershell
# Choose an output parent folder; it must already exist
Start-Process .\Extract.exe -ArgumentList '--output "D:\Unpacked" "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait

# Extract without a notification; logging remains enabled
Start-Process .\Extract.exe -ArgumentList '--quiet "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait

# Open the latest result or job record
.\Extract.exe --open-last-result
```

| Option | Purpose |
| --- | --- |
| `--output <directory>` | Write results under an existing parent directory |
| `--quiet` | Suppress notifications for this run |
| `--list <package>` | Write one package's file list to standard output as JSON without extracting files; process logging remains enabled |
| `--open-last-result` | Open the latest result or job record |
| `--repair-notifications` | Update notification registration after moving the portable folder |
| `--unregister-notifications` | Remove notification registration for the current user |
| `--help` / `--version` | Show help or version information |

The first notification creates notification registration for the current user. Scripts that read the exit code should wait for the process to finish: `0` means success; `299` means partial completion or a batch with both successes and failures.

## Development and further reading

Extract uses **C++20 / C17, CMake, and Win32**. Package parsing and extraction logic are implemented by this project; low-level compression libraries are linked statically.

The detailed developer documents below are currently in Chinese.

- [Build and development](docs/05-build-and-development.md) — toolchain, build commands, and automated tests.
- [Release workflow](docs/16-release.md) — version-tag-only builds for x86, x64, and ARM64; ordinary pushes do not publish releases. ARM64 runtime testing is pending.
- [Product requirements](docs/02-requirements.md) · [Technical design](docs/03-technical-design.md) — behavior, output layout, and resource limits.
- [Roadmap](docs/04-roadmap-and-acceptance.md) — planned formats and features.
- [Third-party libraries and licenses](third_party/README.md) — versions, sources, and attribution.

Only regular files on local drives are supported. There is **no fixed package-size, total-output-size, or file-count ceiling**. Extraction checks available disk space and integer ranges; actual format, decoder, memory, and address-space limits still apply. Metadata and parser work budgets remain in place. Automatic nested extraction is limited to **32 packages and 4 levels**, with cycle detection. See [large packages and resource handling](docs/17-large-packages.md).
