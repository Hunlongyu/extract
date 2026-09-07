# MSI/CAB 优化与 Burn 实现

日期：2026-09-07。对应 Extract 0.5.0。

本轮扩展已有 MSI/CAB 能力，并新增 Burn 静态提取。解析和路径规划由本项目 C++20 模块实现，CAB 解压使用 Windows FDI，XML 使用系统 XmlLite，哈希使用 BCrypt；发布程序没有新增外部解包工具依赖。

## 已实现范围

| 模块 | 0.4.0 | 0.5.0 |
| --- | --- | --- |
| MSI 媒体 | 单个内嵌 CAB | 多个独立 CAB、内嵌/外置 CAB |
| MSI 源文件 | 压缩文件 | 压缩、松散、混合，源/目标目录与长短源名称 |
| MSI 特殊布局 | 较保守的键和媒体要求 | 最长 255 字符标识，超长冲突目录使用摘要名称；松散文件可共用序号；空 File 表输出零载荷说明 |
| 标准 CAB | MSI 内部解码组件 | 独立 `.cab` 入口与 PE 附加区 CAB，自研文件表和路径规划 |
| WiX Burn | 未实现 | `.wixburn` 布局 2、CAB 容器、v3/v4 清单、载荷映射与校验 |
| 外置来源 | MSI 不支持 | MSI 同目录 CAB 和相对源目录；Burn 分离容器与本地外置载荷 |
| 递归预算 | 16 个包 | 32 个包，以容纳多组件 Burn；仍为 4 层、10000 文件、8 GiB，均按根任务累计 |

MSI 按 `Sequence`、`Media.LastSequence` 分配媒体，按 CAB 成员中的 File 标识关联输出。多个独立 CAB 无需把文件拼接成一个大内存流。松散文件使用 Summary Word Count、File 压缩标志、Directory 的 `target:source` 与 `short|long` 规则；本地缺失来源给出具体文件名。输入和祖先目录保持句柄保护，拒绝重解析点。

标准 CAB 支持结构 1.3，无压缩、MSZIP、LZX 已验证。解析器检查头部、文件表、压缩数据偏移、编码和大小，再由 FDI 解码。CAB 成员名称不能直接决定任意输出路径；仍经共享路径规则与冲突处理。标准 CAB 与 InstallShield 私有 `.cab` 分开处理。

## Burn 输出与完成状态

引导文件保存到 `bootstrapper`，安装载荷保存到 `packages/<容器标识>`；同名条件变体放入 `_variants`，保留原始目标名称。识别到的内嵌 MSI/EXE 继续交给已有处理器。原始子安装包保留。

处理器区分 UX 容器、原始引擎签名、附加容器和最终 PE 证书的偏移；从 UX CAB 的成员 `0` 读取 XML。清单的 `SourcePath` 对应 CAB 内部成员编号，`FilePath` 对应逻辑名称。禁止 DTD，限制 XML 大小、节点、深度、属性和容器数量。容器与载荷分别核对存在的 SHA-1、SHA-256 或 SHA-512；所有最终文件另记 SHA-256。

| 情况 | 结果 |
| --- | --- |
| 所有声明文件均可读取、校验通过且内层完成 | `complete`，退出码 0 |
| 外置容器或在线载荷在本地缺失 | 提取已有内容，`contentComplete=false`，`partial`，退出码 299，记录缺失项 |
| 来源存在但大小、哈希、结构不符 | 本层失败并回滚，不作为普通缺失忽略 |
| 子包无法处理或超过累计预算 | 保留已完成外层，记录子状态，退出码 299 |
| MSI 的 File 表为空 | 输出零文件清单，说明没有应用文件载荷；不执行配置动作 |

不会运行 bundle `/layout`、安装链、CustomAction 或输出中的 EXE/DLL，不下载在线载荷。包内摘要提供内容一致性核对；若只给证书条件而没有摘要，则明确说明未验证证书信任，不把自产 SHA-256 当作生产方签名校验。

MSI/Burn 可能依赖不同的同目录文件，因此递归时不复用仅按输入包摘要缓存的提取结果。

## 验证

Debug 与 Release 的 7 项 CTest 均已验证通过：路径、MSI、CAB/Burn、Inno、NSIS、嵌套和归档。最后的长标识冲突目录修正另在两种构建补跑路径、MSI 和 CAB/Burn 三项，均通过。格式测试使用已知原始文件进行大小与摘要对照，不执行生成的安装包。

- MSI：内嵌/外置单 CAB、多 CAB、不同 CAB 成员顺序、MSZIP/LZX、松散/混合源、短源名称、共用序号、长标识、空 File 表；16 个异常包覆盖缺失、损坏、哈希不符、路径越界、源目录重解析点等，检查失败回滚。
- CAB/Burn：39 个结构样本，覆盖 stored/MSZIP、UTF-8 名称、空文件、冲突、CAB SFX、CAB 嵌套、v3/v4 命名空间、原始/最终签名偏移、分离容器、在线缺失、哈希/大小/索引损坏、未映射成员、整数溢出及 DTD 拒绝。
- 嵌套：32 包、4 层、文件数、总字节预算以及路径/损坏状态传播。

### 官方 Python Burn

样本：[Python 3.13.7 Windows x64](https://www.python.org/downloads/release/python-3137/)，从 python.org 官方地址取得。文件的 Authenticode 校验为 Valid，签名方 Python Software Foundation；此项为测试时单独核验，不是当前产品内置的信任验证能力。

输入 SHA-256：`b12e2e82461ac8e51fc43289050bc8eb937a32d84ce4d242e2c88258c37cf2bb`。

| 检查 | 结果 |
| --- | --- |
| 外层文件 | 34 个，63,278,360 字节 |
| 清单载荷独立复核 | 28 个载荷；Python 脚本另读 XML，比对名称、大小与包内哈希 |
| 内层 MSI | 22 个均完成 File 表处理，其中 6 个无 File 表载荷 |
| 各层累计输出 | 8,530 个文件，341,474,642 字节；逐文件 SHA-256 复核 |
| 缺失来源 | 30 个外置载荷引用，包含调试、自由线程等组件及条件变体 |
| 最终状态 | `partial` / 299；内嵌部分完成，包外文件未伪造或下载 |

真实样本使用 v3 清单。v4 命名空间和 SHA-512 已通过已知内容结构样本验证，尚未用真实 v4/v5/v6 编译器产品建立完整版本矩阵，不能宣称全部 WiX 产品版本兼容。

### 用户目录回归

对 `out/dist/win-x64-release` 下原有 5 个安装包使用 Release 程序重新提取；检查每层大小和 SHA-256，7z 另对照官方 `7zr` 的清单/CRC，Inno 另读位置表摘要。所有输入摘要保持不变。

| 安装包 | 累计文件数 | 累计字节 | 结果 |
| --- | ---: | ---: | --- |
| ECHO-NEXT-Setup-26.8.2.exe | 441 | 1,159,296,535 | 应用已提取；卸载器目标路径未定，299 |
| vic-diary-2.0.0-setup.exe | 100 | 328,953,160 | 应用已提取；卸载器目标路径未定，299 |
| Pebble_0.1.4_x64-setup.exe | 7 | 16,212,862 | 完整，0 |
| Textify.exe | 6 | 236,025 | 完整，0 |
| XnViewMP_v1.11.6.0_setup_x64_mefcl.exe | 1083 | 251,326,796 | NSIS → Inno 完整，0 |

机器可读摘要见 [0.5.0 验证数据](data/validation-0.5.0.json)。完整本地结果保存在 `out/installers-0.5.0-release.json`、`out/burn-0.5.0-release.json` 及对应提取目录，不随产品发布安装包样本。

复现命令：

```powershell
.\scripts\build.ps1 -Configuration Debug -Test
.\scripts\build.ps1 -Configuration Release -Test -Install
python tests/verify-burn.py --executable out/build/win-x64-release/bin/Extract.exe `
    --input out/samples/burn/python-3.13.7-amd64.exe `
    --output-parent out/burn-validation --report out/burn-validation.json
```

## 当前限制与后续

多个独立 CAB 已支持；一个文件跨 CAB 续接仍不支持。MSI 管理安装映像、MST/MSP、Media.Source 的属性/URL 重定位、安装动作和 Binary 辅助流导出未实现。CAB SFX 只扫描 PE 附加区起始 64 KiB 内的标准 CAB，不覆盖任意私有封装。

输入包限 512 MiB；MSI 单个 CAB 限 256 MiB、外置 CAB 累计 512 MiB。Burn 限 128 个容器、8 MiB XML、128 个实际外置输入且合计 512 MiB。CAB 文件名元数据累计 8 MiB、FDI 分配 64 MiB。全树输出仍受 8 GiB / 10000 文件限制。工作进程与硬超时、MSIX/APPX 专用校验和现代更新框架继续作为后续工作。

构建和自动提取验证不等于软件可便携运行，也不代替干净 Windows 环境中的 Explorer 拖拽和通知横幅人工验收。

## 参考依据

MSI 语义参照 Microsoft 的 [File 表](https://learn.microsoft.com/en-us/windows/win32/msi/file-table)、[Directory 表](https://learn.microsoft.com/en-us/windows/win32/msi/directory-table)、[Media 表](https://learn.microsoft.com/en-us/windows/win32/msi/media-table)和[压缩/非压缩来源](https://learn.microsoft.com/en-us/windows/win32/msi/compressed-and-uncompressed-sources)。

Burn 布局和映射思路参照生产方 WiX 源码：[BurnCommon.cs](https://github.com/wixtoolset/wix/blob/77aa9818ad37637f961afe143be88bdc38a3f350/src/wix/WixToolset.Core.Burn/Bundles/BurnCommon.cs)、[BurnReader.cs](https://github.com/wixtoolset/wix/blob/77aa9818ad37637f961afe143be88bdc38a3f350/src/wix/WixToolset.Core.Burn/Bundles/BurnReader.cs)。参考快照为 `77aa9818ad37637f961afe143be88bdc38a3f350`；这些 C# 提取程序没有被嵌入或作为运行依赖。
