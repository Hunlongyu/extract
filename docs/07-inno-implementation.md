# Inno 实现与验证

日期：2026-09-05。版本：0.2.0。此阶段在现有 MSI 能力上增加 Inno 内嵌静态文件提取；NSIS / Electron 为下一格式阶段。

本文件保留 0.2.0 的范围、验证与产物摘要。当前支持版本见 [Inno 兼容性](20-inno-compatibility.md)。0.2.1 新增 `6.1.0 (u)` / loader v1，见 [Textify 兼容修复](08-textify-compatibility.md)；0.3.1 新增 `6.5.0` 与内嵌安装包自动展开，见 [嵌套提取](10-nested-extraction.md)。

## 已实现范围

| 精确数据版本 | 已验证的官方编译器 | 安装器布局 |
| --- | --- | --- |
| `6.5.2` | 6.5.4 | PE32 / x86 |
| `6.6.1` | 6.6.1 | PE32 / x86 |
| `6.7.0` | 6.7.3 | PE32 / x86 |
| `7.0.0.3` | 7.1.0 | PE32 / x86 和 PE32+ / x64 |

以上各项覆盖无压缩、Deflate、bzip2、LZMA、LZMA2，以及独立块和 solid 流。产品版本和数据结构版本不同；其它标识明确返回不支持，不用最接近的已知布局猜测。

中文、补充平面 Unicode、空文件、重复源引用、同路径条件变体、文件/目录前缀冲突均保留。`dontcopy` 的内嵌文件也可导出。`{app}`、`{tmp}` 等前置常量映射到输出中的逻辑目录；Components、Tasks、Languages、Check 表达式保存在报告中，不执行条件或脚本。

动态目录（例如 `{code:GetDir}`）的文件存入 `_unresolved/<条目 ID>/`，保留原始 `sourceExpression`，报告 `status=partial`、`pathsResolved=false` 并返回 299。文件仍要完整通过包内 SHA-256 校验。普通路径的同名/前缀冲突由 MSI 与 Inno 共用的规划器保存到 `_variants`。

## 解析与解压流程

1. `io::Input` 持有只读输入、祖先目录句柄与文件映射；按内容分派 MSI 或 PE。没有调用 `LoadLibrary` 加载输入 EXE。
2. 自有 PE 读取器检查 DOS/PE32/PE32+、节表与资源树边界，通过 RCDATA / 11111 定位 Inno loader 版本 2 的 64 字节偏移表，验证其 CRC。
3. 读取 loader 声明的 payload、metadata 偏移和最小文件长度。再读取精确数据版本与带 CRC 的加密描述；本版对加密和外置卷返回不支持。
4. 读取两块元数据：块头 CRC，随后每段不超过 4096 字节的内容 CRC。按版本区分 32/64 位的 StoredSize；元数据可以不压缩或使用 LZMA。
5. 按磁盘顺序读取长度前缀 UTF-16 字符串与固定尾部，跳过文件表之前的语言、消息、权限、类型、组件、任务、目录与签名键表。按版本适配头字段、语言布局、组件层级宽度与文件标志位大小。
6. 文件表关联位置表，检查位置范围、块签名、重复块描述、固实子偏移、文件引用与输出预算。安装时生成的卸载程序记录单独说明，不伪造静态文件。
7. 每个压缩块仅解码一次；用 64 KiB 缓冲分发文件与别名。根据标志恢复 Inno CALL/JMP v3 变换，以文件为边界重置偏移。处理完声明内容后检查流结束标记与剩余输入，拒绝短流或额外展开内容。
8. 每个导出文件都与位置表中的 SHA-256 比对，通过后保存 `_extract-report.json` 并提交独立输出目录。成功只表示内嵌静态文件已提取，不表示完整安装环境已经恢复。

主要依据为生产方固定 tag 的 [Shared.Struct.pas](https://github.com/jrsoftware/issrc/blob/is-7_1_0/Projects/Src/Shared.Struct.pas)、[Shared.SetupEntFunc.pas](https://github.com/jrsoftware/issrc/blob/is-7_1_0/Projects/Src/Shared.SetupEntFunc.pas)、[Compression.Base.pas](https://github.com/jrsoftware/issrc/blob/is-7_1_0/Projects/Src/Compression.Base.pas) 与 [Setup.FileExtractor.pas](https://github.com/jrsoftware/issrc/blob/is-7_1_0/Projects/Src/Setup.FileExtractor.pas)。旧布局分别核对 is-6_5_4、is-6_6_1、is-6_7_3；没有将第三方完整解析器改名或包装后引入。

自有 C++20 代码负责容器结构与提取流程；LZMA SDK 26.03、zlib 1.3.2、bzip2 1.0.8 仅提供压缩基础算法。30 个引入的源码/头文件逐个与原始发布包比较，内容未修改。来源与许可见 [基础库记录](../third_party/README.md)。

## 自动测试

`tests/inno_integration.py` 先用官方 ISCC 编译已知内容，再由 Extract 提取，逐文件比较原始字节与 SHA-256。不执行生成的安装器或其载荷。已提取且签名有效的官方 ISCC 可作为开发工具生成样本，不进入发行依赖。

每个编译器版本覆盖：

- 五种压缩 × solid 开关；7.1 另覆盖 x86/x64，共 50 个基础配置。
- 150,008 字节固定内容，在多个 64 KiB 边界放入正/负 CALL/JMP 操作数；配合空文件、同源别名、nocompression 与 solidbreak。
- 中文和 emoji 路径；非空的类型、组件、任务、目录与消息表；同目标条件文件和目录前缀冲突。
- 清单模式不产生输出、源安装包哈希不变、重复输出不覆盖、动态目录报告部分完成、dontcopy 文件。
- 33 个拒绝样本：外部文件、加密、未知版本、loader/元数据/加密描述 CRC、截断元数据或各压缩流、字符串长度/UTF-16、文件哈希、位置偏移/长度/标志、输出和字典预算、分卷、路径穿越/ADS/设备名、PE 偏移/节数。
- 损坏包与有效包混合批次返回 299，并继续提取有效包；拒绝样本不提交目录，也不遗留本次暂存目录。

每个编译器另生成不压缩元数据、动态路径和 dontcopy 三个有效样本；总计 62 种有效配置和 132 个拒绝样本。Debug、Release 分别运行。结果保存在各测试目录的 `validation.json`；默认 CTest 使用本机 6.7.3，其它编译器通过显式 `--compiler` 运行。MSI 与路径测试同时回归。

统一输出在本轮回归中出现过短暂共享占用导致的目录提交失败。现在仅对同一句柄的重命名做最多 2 秒有限重试；不改变文件权限、不覆盖已存在目标。真实持有文件句柄 250 ms 的测试已覆盖该行为。达到重试上限时仍保留暂存结果并报告位置。

## 真实安装包

以下均从生产方 GitHub Release 下载，下载摘要与官方资产记录核对。每个静态文件都通过包内 SHA-256 校验。

| 官方样本 | 数据版本 | 文件数 | 文件总字节 |
| --- | --- | --- | --- |
| [innosetup-6.5.4.exe](https://github.com/jrsoftware/issrc/releases/tag/is-6_5_4) | 6.5.2 | 115 | 20,134,579 |
| [innosetup-6.6.1.exe](https://github.com/jrsoftware/issrc/releases/tag/is-6_6_1) | 6.6.1 | 119 | 25,982,592 |
| [innosetup-7.1.0-x64.exe](https://github.com/jrsoftware/issrc/releases/tag/is-7_1_0) | 7.0.0.3 | 135 | 52,674,963 |

输入 SHA-256：

```text
6.5.4     fa73bf47a4da250d185d07561c2bfda387e5e20db77e4570004cf6a133cc10b1
6.6.1     d243ce440c02705530699554fb9612b9b2bd7a2a90629cdb7f41e66f5faeb91f
7.1.0-x64 0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f
```

7.1 的 69 个输出文件另与该官方 tag 的 `Files/` 目录逐字节摘要比对一致。各版本提取的 ISCC 签名有效；7.1 的 ISCmplr / ISPP 签名也已检查。未运行安装流程；编译器仅在开发测试时显式用于构建惰性样本。

这些真实样本验证 Inno 结构，不代表已经测过所有近期软件，也不构成近期软件成功率统计。

手动复验只读取输入与已经提取的文件，示例：

```powershell
.\tests\verify-inno.ps1 -Version 7.1.0-x64 `
    -Package out/samples/innosetup-7.1.0-x64.exe `
    -OutputDirectory out/samples/innosetup-7.1.0-x64_extracted `
    -ReferenceFiles out/references/issrc-7.1/Files
```

## 便携构建

本机 Debug/Release 的三项 CTest 通过，另外分别运行 6.5.4、6.6.1、7.1.0 矩阵。每种构建共 268 次 Inno 清单/提取调用。

Release 程序位于 `out/dist/win-x64-release/Extract.exe`，版本 0.2.0，427,008 字节，SHA-256：

```text
8931b995e5070d376e00e6b073b56acfdeb45626a1ecf162f19cea62ea14e54c
```

PE 检查为 AMD64 / Windows GUI，ASLR、DEP、CFG 开启；导入表只包含 Windows 系统库，无 MSVC 动态 CRT 或第三方 DLL。便携目录同时包含文档和三个基础库的许可。

从该便携目录重新提取以上三个真实 Inno 样本，共 369 个文件全部复验通过；7.1 的 69 个生产方文件也再次比对一致。通知注册已指向当前便携程序，协议冷启动探针通过，未打开任何输出目录。此探针验证系统激活链路，不代表通知横幅的视觉验收。

## 当前限制与后续

- 取消输入 512 MiB、文件/表条目 10,000 及输出 8 GiB 固定阈值；大小累加检查 64 位溢出，表计数与实际元数据长度核对。元数据每块压缩/展开各 64 MiB，LZMA 字典 256 MiB、单解码器分配 264 MiB。不是整进程硬内存限制。
- 外置/跨卷、加密、下载文件和不在精确版本表中的结构返回不支持。更早的近期 6.2–6.4 与其它 6.5/7.0 标识仍待样本驱动适配。
- 不执行 Pascal、BeforeInstall/AfterInstall、注册和运行指令；不生成卸载程序、空目录或运行时文件，不恢复权限/时间戳，不自动展开内嵌归档。
- 不模拟安装选择及架构分支。报告保留全部静态文件与已读取条件，路径是逻辑路径，不是用户机器的真实安装目录。
- 仍为主进程顺序处理，没有 worker 或硬超时；尚未做持续模糊测试、干净 Windows 10/11 矩阵或 Explorer 拖拽/通知点击的完整视觉验收。
- 下一格式为 NSIS 3.x，再用近期 Electron 离线包验证内外层载荷；不会因为本轮引入 LZMA SDK 就宣称已经支持 NSIS 或独立 7z。
