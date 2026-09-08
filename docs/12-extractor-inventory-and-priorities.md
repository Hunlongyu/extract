# 本地解包工具清点与自主实现优先级

核查日期：2026-09-05。项目基线：Extract 0.4.0。本文是调查与后续建议，没有新增解包功能，也没有运行两个目录里的工具或目标安装包。

## 结论

优先完善现有 MSI/NSIS，再新增标准 CAB 和 WiX Burn；随后做 MSIX/APPX、Squirrel/Velopack 离线包，以及有近期样本的 InstallShield 封装。无需把整个 UniExtract 工具箱重写一遍。

以上为调查时的顺序。当前 MSI/CAB/Burn 已实现，Velopack/Squirrel、MSIX/APPX/Bundle 首批离线布局及 Inno 外置卷、独立/MSI CAB 跨卷续接也已在本地实现；最新范围见 [更新包实现](24-update-packages.md)、[MSIX/APPX 实现](25-msix-appx.md)与 [分卷实现](26-split-volumes.md)。

“近期”按本项目既定的 2021–2026 年样本范围；下文优先级依据现有代码缺口、格式组合价值和实现范围判断，**不是安装器市场占有率统计**。

安装包解析、目录规划、嵌套提取由我们实现；压缩/通用归档基础库、Windows CAB API 可以继续使用。参考工具仅用于理解结构和建立测试对照，不作为发布程序的外部依赖。

## 核查口径与数量

| 代号 | 本地路径 | 全部文件 | EXE 文件 | 说明 |
| --- | --- | ---: | ---: | --- |
| A | `D:\Software\UniExtract\bin` | 193 | 79 | 多个 x86/x64 工具与游戏提取器 |
| B | `D:\Software\Universal Extractor\bin` | 2,094 | 59 | 包括 `die/db` 下 1,947 个规则、脚本、图标等配套文件 |
| 合计 | 两目录递归清点 | 2,287 | 138 | EXE 按不区分大小写的文件名去重为 **94 种**；同名不保证同版本或相同内容 |

下表覆盖这 94 个不同 EXE 文件名，按用途合并同族工具；另列全部 19 个不同 Observer `.so` 模块和 9 个不同 WCX 插件。DLL、配置、规则库不能当成不同解包格式。

- [340 条工具及配套文件清单](data/12-tool-file-inventory.csv)：保留相对路径、字节数、版本资源、SHA-256、用途和建议；仅排除已汇总的 1,947 个 DiE 数据文件。
- 完整 2,287 条文件快照：本地 `out/extractor-inventory/full-file-inventory.csv`；原始元数据：`out/extractor-inventory/raw.json`。
- 证据来自本地作者说明、`def/*.ini`、`observer.ini`、PE 版本资源及必要的静态字符串；对照 UniExtract2 上游源码，提交 `9719ac988421e48276420e2f33e09087cfbacf8d`。没有加载未知 DLL。
- 这些记录证明文件存在及用途；**没有验证每个工具能正常启动，也没有验证其对当前所有格式版本的兼容性**。

## 版本和容易混淆的名称

| 工具 | A 的本地版本资源 | B 的本地版本资源 |
| --- | --- | --- |
| 7-Zip | 19.00，x86/x64 两份 | 25.01 |
| lessmsi | 1.0.10.13880 | 2.10.4 |
| WiX dark | 3.11.2.4516 | 3.14.1.8722 |
| EnigmaVBUnpacker | 0.5.8.0 | 0.6.3.0 |
| Exeinfo PE | 0.0.6.0 | 0.0.9.3 |
| DiE | 未找到 | 3.10.0.0 |
| innounp | 2.71.1 | 此 bin 树未找到同名工具 |

版本来自文件资源，不把上游工具表的版本套到本地副本上。A 的 `innoextract.exe` 无版本资源，本轮不执行它来询问版本。B 没有同名独立工具不代表其主程序绝对无法通过其它途径处理 Inno。

需要特别区分：`MsiX.exe` 解的是 **MSI**；`PDunSIS.wcx` 解的是 **Symbian SIS**；`VIS3Ext.exe` 解的是 **Visionaire 游戏资源**。它们分别不是 MSIX、NSIS 和 Installer VISE。

## 逐项工具用途

“A / B”只表示对应文件在两个目录中都找到；不表示版本或能力相同。


### 安装包与封装

| 工具 | 所在目录 | 用途 | 对 Extract 的建议 |
| --- | --- | --- | --- |
| `innounp.exe`、`innoextract.exe` | innounp: A；innoextract: A | Inno Setup 安装包：文件表、路径、压缩块、版本差异。 | 已有模块；持续补近期结构、分卷和路径变体。 |
| `lessmsi.exe`、`jsMSIx.exe`、`MsiX.exe` | lessmsi: A/B；jsMSIx: A/B；MsiX: A/B | Windows Installer MSI/MSM 内容提取；其中 MsiX 是旧工具名称，不是现代 MSIX 格式。 | 已有 MSI；优先补多 CAB、外置 CAB、松散源文件。 |
| `dark.exe` | dark: A/B | WiX 反编译工具；可提取 Burn Bundle 中的附带容器和载荷，也能处理 MSI 反编译。 | 优先新增 Burn 静态解析；不需要生成 WiX 工程。 |
| `unshield.exe` | unshield: A/B | InstallShield 专有 CAB/HDR 文件表、组件、文件组和跨卷载荷。 | 值得自研，列入第二批；以近期样本确定结构。 |
| `i5comp.exe`、`i6comp.exe` | i5comp: B；i6comp: A/B | InstallShield 5.x / 6.x 专有 CAB 的提取、维护工具。 | 参考数据结构；不专门重做这两个历史版本的完整工具。 |
| `i3comp.exe`、`STIX_D.EXE` | i3comp: B；STIX_D.EXE: B | InstallShield 3.x 的 DATA.Z / DATA.1 等旧归档及部分 SFX。 | 不纳入近期安装包主线。 |
| `IsXunpack.exe` | IsXunpack: A/B | 部分 InstallShield 单 EXE 外壳：取出 CAB/HDR、setup 文件，未必已经取出应用。 | 参考外壳到载荷的分层设计；新版本必须另做样本验证。 |
| `E_WISE_W.EXE`、`WUN.exe` | E_WISE_W.EXE: A/B；WUN: A/B | Wise Installer 静态载荷提取；E_WISE.INI 提供配套信息。 | 低优先级；遇到实际近期样本再排期。 |
| `cicdec.exe` | cicdec: A | Clickteam Install Creator 安装包。 | 候选扩展；有近期失败样本再投入。 |
| `sim_unpacker.exe` | sim_unpacker: B | Smart Install Maker 安装包。 | 候选扩展；先借助标准 CAB 能力评估载荷。 |
| `spoondec.exe` | spoondec: A | Spoon Installer 安装包载荷。 | 低优先级；不要与所有应用虚拟化封装混为一谈。 |
| `RAIU.EXE` | RAIU.EXE: A/B | Reflexive Arcade 游戏安装器的外层包装，通常还要接 Inno 提取。 | 不做历史专用分支；参考多层识别思路即可。 |
| `AFPIunpack.exe` | AFPIunpack: B | Adobe Flash Player 安装器解包。 | 不纳入主线。 |
| `Expand.exe` | Expand: B | Windows 压缩文件/CAB 展开工具；本地携带的是 Windows 7 版本。 | 实现独立标准 CAB 入口；直接使用系统 FDI，已有基础可复用。 |
| `7ZSplit.exe`、`SfxSplit.exe` | 7ZSplit: A；SfxSplit: B | 7z SFX 的模块、配置与归档拆分；SfxSplit 的作者信息指向 7zsfx。 | 已有 PE + ZIP/7z；补识别和精确边界，不重做一套压缩引擎。 |

### 通用归档

| 工具 | 所在目录 | 用途 | 对 Extract 的建议 |
| --- | --- | --- | --- |
| `7z.exe` | 7z: A/B | 通用归档读取；完整 7-Zip 还支持许多安装器、镜像等容器。 | 借鉴识别和边界检查；本项目现有 7z SDK 子集不等于完整 7-Zip 能力。 |
| `unzip.exe` | unzip: A/B | ZIP 及部分 ZIP 自解压包。 | 已有 ZIP/ZIP64；无需再做另一个 ZIP 工具。 |
| `UnRAR.exe` | UnRAR: A | RAR 和 RAR 自解压归档。 | 通用下载包的候选增强；使用原生归档基础库，无需自写 RAR 算法。 |
| `unarc.exe` | unarc: A/B | FreeArc 归档；与下面 DOS ARC 同扩展名但格式不同。 | 低优先级，面向特定重打包样本再评估。 |
| `arc.exe` | arc: A/B | 传统 DOS ARC 归档。 | 不做。 |
| `arj.exe` | arj: A | ARJ 归档及相应自解压包。 | 不做历史格式专项。 |
| `acefile.exe`、`xace.exe` | acefile: A；xace: B | ACE 归档，两套工具。 | 不做历史格式专项。 |
| `unalz.exe` | unalz: A/B | ALZip 的 ALZ 归档。 | 低优先级。 |
| `kgb2_console.exe` | kgb2_console: A/B | KGB 归档，paq0–7.dll 为配套算法模块。 | 不做。 |
| `UHARC02.EXE`、`UHARC04.EXE`、`UNUHARC06.EXE` | UHARC02.EXE: A；UHARC04.EXE: A/B；UNUHARC06.EXE: A/B | UHARC 的不同历史版本。 | 不做。 |
| `unlzx.exe` | unlzx: A/B | 传统 LZX 归档，不能据此认为它是标准 CAB 的 LZX 接口。 | 不做独立格式专项。 |
| `unzoo.exe` | unzoo: A/B | Zoo 归档。 | 不做。 |
| `bcm.exe` | bcm: A | BCM 压缩格式。 | 低优先级。 |
| `dgcac.exe` | dgcac: B | DGCA / DGC 归档。 | 低优先级。 |
| `lzip.exe`、`lzop.exe` | lzip: A；lzop: A/B | 分别为 LZIP 和 LZO/lzop 压缩流。 | 安装器确有这种压缩方法时再接基础库；不单列首批格式。 |
| `pea.exe` | pea: A/B | PEA 压缩、加密和认证容器；不是完整 PeaZip 界面的全部后端。 | 低优先级。 |
| `unar.exe`、`Expander.exe` | unar: A；Expander: B | 前者是 The Unarchiver 命令行、多格式读取；本地路由用于 StuffIt。后者是旧 StuffIt Expander。 | 不因同样能解压就重做；StuffIt 不纳入 Windows 安装包主线。 |
| `zpaq.exe` | zpaq: A/B | ZPAQ 归档/增量备份。 | 低优先级。 |

### 识别、宿主及可执行程序封装

| 工具 | 所在目录 | 用途 | 对 Extract 的建议 |
| --- | --- | --- | --- |
| `diec.exe` | diec: B | Detect It Easy 命令行识别器；检测文件格式、编译器、安装器和壳。 | 参考针对安装器的少量签名与结构验证，不移植整套规则引擎。 |
| `exeinfope.exe`、`PEiD.exe` | exeinfope: A/B；PEiD: A/B | PE 文件、编译器和壳的识别；不是应用文件提取器。 | 参考识别特征；识别结果只能作为解析候选。 |
| `trid.exe`、`file.exe` | trid: A/B；file: A | TrID 基于特征库判断文件类型；file/libmagic 使用 magic 规则。 | 做少量确定性探测，未知包保存诊断；不追求识别所有文件。 |
| `cmdTotal.exe` | cmdTotal: B | 命令行 WCX 插件宿主，调用指定插件列出/提取文件。 | 无需重做插件 ABI；沿用我们的格式模块接口。 |
| `mtee.exe` | mtee: A/B | 复制命令行输出到日志等目的地。 | 已有日志功能，无需独立程序。 |
| `xor.exe` | xor: A/B | 通用 XOR 字节变换辅助程序。 | 确有封装字段需要时在对应解析器内实现。 |
| `EnigmaVBUnpacker.exe` | EnigmaVBUnpacker: A/B | Enigma Virtual Box 单文件虚拟文件系统的静态拆包。 | 可选扩展，排在常见安装包之后；提取完成不代表程序能脱离虚拟化运行。 |
| `demoleition.exe` | demoleition: A | MoleBox 2.x 静态拆包。 | 不纳入近期安装包主线。 |
| `Extractor.exe` | Extractor: B | h4sh3m Thinstall/ThinApp 依赖提取器；上游接法要先启动目标进程。 | 不采用该动态提取路径。 |
| `upx.exe` | upx: A/B | UPX 可执行文件压缩/还原，处理程序映像。 | 不是安装包文件表；不做通用脱壳功能。 |
| `AspackDie.exe`、`AspackDie22.exe` | AspackDie: A/B；AspackDie22: B | ASPack 可执行文件脱壳；配套 ForceLibrary.dll。 | 不做。 |
| `Exe2Aut.exe` | Exe2Aut: B | AutoIt 编译脚本反编译。 | 不做源代码恢复；与安装包载荷提取区分。 |

### 镜像与固件

| 工具 | 所在目录 | 用途 | 对 Extract 的建议 |
| --- | --- | --- | --- |
| `daa2iso.exe`、`uif2iso.exe` | daa2iso: A/B；uif2iso: A/B | 分别把 DAA、UIF 镜像转换成 ISO。 | 不做专用转换器。 |
| `cdirip.exe` | cdirip: B | DiscJuggler CDI 光盘镜像拆分。 | 不纳入主线。 |
| `unisz.exe` | unisz: A | UltraISO ISZ 压缩镜像展开。 | 不纳入主线。 |
| `unecm.exe` | unecm: A | ECM 光盘数据恢复。 | 不纳入主线。 |
| `unadf.exe` | unadf: A | Amiga ADF 磁盘镜像。 | 不做。 |
| `bootimg.exe` | bootimg: B | Android boot image 拆分工具；按上游同名工具路由判断，本地具体版本未核实。 | 不做。 |
| `NBHextract.exe` | NBHextract: A/B | HTC NBH ROM 固件。 | 不做。 |

### 游戏与多媒体资源

| 工具 | 所在目录 | 用途 | 对 Extract 的建议 |
| --- | --- | --- | --- |
| `GARbro.Console.exe` | GARbro.Console: A | 多种游戏/视觉小说资源容器，ArcFormats.dll 和 Formats.dat 是配套。 | 不做游戏资源平台；安装包解出游戏文件即可。 |
| `quickbms.exe` | quickbms: A | 脚本驱动的二进制格式解释器；本地 BMS.db 提供解包脚本集合，也有少数安装器脚本。 | 仅参考确实命中安装器的脚本；不重做整个解释器。 |
| `bsab.exe` | bsab: A | Bethesda BSA/BA2 游戏资源。 | 不做。 |
| `Champollion.exe` | Champollion: A | Papyrus PEX 游戏脚本反编译。 | 不做。 |
| `godotdec.exe` | godotdec: A | Godot PCK 及内嵌 PCK 的 EXE。 | 不做游戏内部资源展开。 |
| `RgssDecrypter.exe`、`rmvdec.exe` | RgssDecrypter: A；rmvdec: A | 分别处理 RPG Maker RGSS 归档和 RPG Maker MV 资源。 | 不做。 |
| `unrpa.exe` | unrpa: A | Ren'Py RPA 归档。 | 不做。 |
| `ttarchext.exe` | ttarchext: A | Telltale TTARCH 游戏资源。 | 不做。 |
| `utagedec.exe` | utagedec: A | UTAGE 游戏资源。 | 不做。 |
| `VIS3Ext.exe` | VIS3Ext: A | Visionaire Engine 3/4 VIS 游戏资源；不是 Installer VISE。 | 不做。 |
| `fsbext.exe` | fsbext: A | FMOD FSB 音频资源包。 | 不做。 |
| `sfarkxtc.exe` | sfarkxtc: A | sfArk 压缩音色库。 | 不做。 |
| `swfextract.exe` | swfextract: A/B | SWF 图片、音频等资源。 | 不做。 |

### 文档、邮件与数据转换

| 工具 | 所在目录 | 用途 | 对 Extract 的建议 |
| --- | --- | --- | --- |
| `pdfdetach.exe`、`pdfimages.exe`、`pdftotext.exe`、`pdftohtml.exe`、`pdftopng.exe` | pdfdetach: A/B；pdfimages: B；pdftotext: A/B；pdftohtml: A；pdftopng: A | 分别提取 PDF 附件、图片、文本，转换 HTML、渲染 PNG。 | 不做；保持提取出的 PDF 原样。 |
| `helpdeco.exe`、`clit.exe` | helpdeco: A/B；clit: A/B | 分别反编译 WinHelp HLP、展开 Microsoft Reader LIT 电子书。 | 不做。 |
| `extractMHT.exe` | extractMHT: B | MHT/MHTML 网页归档拆分。 | 不做。 |
| `lconvert.exe`、`msgunfmt.exe` | lconvert: A；msgunfmt: A | 分别转换 Qt QM 翻译文件、GNU Gettext MO 翻译目录。 | 不做。 |
| `sqlite3.exe` | sqlite3: A | SQLite 数据库访问/导出；也可供工具管理自身数据。 | 不做数据库转换功能。 |
| `uudeview.exe` | uudeview: A/B | UU/XX/Base64 等编码内容解码。 | 不做独立文档格式；封装确有字段需要时局部实现。 |

## 插件与运行库

### WCX 插件

| 插件 | 目录 | 用途 |
| --- | --- | --- |
| `InstExpl.wcx` | A / B | InstallExplorer：Wise、VISE、Inno、Gentee、InstallShield、旧 NSIS、SetupFactory、Eschalon、MSI 的多格式插件 |
| `TotalObserver.wcx` | A / B | Observer 模块的 Total Commander 宿主；实际格式由 modules 和配置决定 |
| `msi.wcx` | A / B | MSI/MSP 数据库内嵌 OLE 流浏览/提取；不能等同按安装目录完整还原应用 |
| `iso.wcx` | A | ISO 光盘镜像插件 |
| `dbxplug.wcx` | A / B | Outlook Express DBX 邮件库插件 |
| `PDunSIS.wcx` | A / B | Symbian/EPOC SIS 安装包插件；不是 NSIS |
| `gaup_pro.wcx` | A | Game Archive UnPacker 游戏资源插件 |
| `MHTUnp.wcx` | B | MHT 网页归档插件 |
| `TotalSQX.wcx` | B | SQX 归档插件；sqx20u.dll 和 sqx2_*.zzl 为配套 |

### Observer 模块

`.so` 在这里是 Windows 插件模块的文件后缀，不等于需要 Linux。按作者说明和配置识别职责，未实际加载。

| 模块 | 目录 | 用途 | 建议 |
| --- | --- | --- | --- |
| `msi.so` | A / B | MSI/MSM | 增强现有 MSI |
| `nsis.so` | A | NSIS 安装包 | 已有；增强现有 NSIS |
| `ishield.so` | A / B | InstallShield CAB/HDR/Z/部分 EXE | 近期结构值得新增 |
| `sfact.so` | A / B | Setup Factory 安装包 | 近期样本驱动的候选 |
| `gentee.so` | A / B | Gentee / CreateInstall 安装包 | 近期样本驱动的候选 |
| `wise.so` | A / B | Wise 安装包 | 低优先级 |
| `isoimg.so` | A / B | ISO 等光盘镜像 | 可选外层载体，后置 |
| `udfimg.so` | A / B | UDF 光盘镜像 | 可选外层载体，后置 |
| `vdisk.so` | A | 虚拟磁盘镜像 | 范围外 |
| `wcx.so` | A | Total Commander WCX 插件桥接 | 无需实现宿主 |
| `pst.so` | A / B | Outlook PST 邮件库 | 范围外 |
| `mbox.so` | A / B | MBox 等邮件容器 | 范围外 |
| `mime.so` | A / B | EML/MHT 等 MIME 内容 | 范围外 |
| `pdf.so` | B | PDF 内嵌文件 | 范围外 |
| `mpq.so` | A / B | Blizzard MPQ 游戏容器 | 范围外 |
| `valve.so` | A / B | Valve/Steam GCF、VPK、PAK、WAD、BSP 等 | 范围外 |
| `relic.so` | B | Relic BIG/SGA 游戏容器 | 范围外 |
| `vp.so` | B | Volition Pack / FreeSpace VP | 范围外 |
| `x23cat.so` | B | Egosoft X 系列 CAT/PCK/PBD/PBB | 范围外 |

A 的 `observer.ini` 引用了不存在的 `vp.so`、`relic.so`、`x23cat.so`；A 实际有 `nsis.so`、`vdisk.so`、`wcx.so`，但未列入当前 `[Modules]`。B 列出的模块文件都存在。因此不能把“磁盘上有某模块”或“配置中有某名字”直接当成可用能力；本轮没有修改配置。

### 配套文件应如何理解

| 文件族 | 作用 | 我们是否要重做 |
| --- | --- | --- |
| `7z.dll` | 完整 7-Zip 格式/压缩引擎 | 不重做整个引擎；现有 SDK 使用范围独立说明 |
| `Unp/Bzip2_*.unp`、`inflate*.unp`、`lzma.unp`、`pkware.unp` | 历史插件的解码辅助模块 | 使用现有原生基础库；PKWARE 方法按具体格式选型 |
| `Unp/Eschalon.unp`、`Gentee.unp`、`vise.unp` | 历史安装器插件的辅助模块 | 有近期样本才单独分析，不能仅据名称承诺支持 |
| `InstExpl.dll`、`Unpack.dll` | 提取工具/插件配套；两个目录中同名文件的大小不同 | 具体 ABI 与调用方尚未逐一核实；不作为独立格式 |
| `LessIO.dll`、`lessmsi.core.dll`、`libmspackn.dll`、`mspack.dll`、`wix*.dll`、`winterop.dll`、`Microsoft.Deployment.*.dll` | lessmsi/WiX 文件访问、安装包、CAB 等运行依赖 | 用自身 MSI 解析层和 Windows API 完成目标 |
| `ZD50149.DLL`、`ZD55131.DLL` | 旧 InstallShield ZData 压缩辅助库 | 不携带旧工具运行库；解析器配原生解码库 |
| `Dpx.dll`、`Msdelta.dll`、`Mspatcha.dll` | Windows 差分包/补丁相关组件 | 后置；只有差分数据且没有基准文件时不能还原完整应用 |
| `paq0.dll`–`paq7.dll`、`sqx20u.dll`、`sqx2_*.zzl`、`stuffit5.engine-5.1.dll` | KGB、SQX、StuffIt 配套引擎/SFX 数据 | 对应格式均非主线 |
| `libbz2*.dll`、`zlib1.dll`、`7z/Codecs/brotli/*.dll` | 通用压缩基础库 | 算法可使用原生库；仅携带 DLL 不代表主程序已注册该格式 |
| `Qt5Core.dll`、`Qt5Script.dll`、`Au3.bin` | DiE / AutoIt 运行时辅助 | 我们的 Win32 程序不需要这些宿主依赖 |
| `Bio.cs.dll`、`ICSharpCode.SharpZipLib.dll`、`RgssDecrypter.Lib.dll` | .NET 工具公共/业务库 | 不引入对应工具体系 |
| `GARbro/*.dll`、`DiscUtils.dll`、`Foundation.1.0.dll` | 游戏资源、虚拟磁盘或 unar 的配套库 | 不扩展这些业务范围 |
| `neko.dll`、`gc.dll`、`gcmt-dll.dll`、`std.ndll`、`regexp.ndll` | Neko 等运行时 | 不需要 |
| `glib-2.0.dll`、`gobject-2.0.dll`、`gmime.dll`、`msvcp100.dll`、`msvcr100.dll` | Observer 插件框架/MIME/旧 VC 运行库 | 不需要复制 |
| `TrIDLib.dll`、`Ext_Detector.dll`、`magic1.dll`、`regex2.dll` | 文件识别器与其依赖 | 按安装器需要实现识别即可 |
| `ForceLibrary.dll` | ASPack 脱壳工具配套的进程 DLL 加载辅助 | 不采用 |
| `triddefs.trd`、`userdb.txt`、`magic*`、`die/db/*` | 格式/壳识别规则和配套数据 | 不等于解压算法 |
| `BMS.db`、`GARbro/GameData/Formats.dat`、`vis.key` | 解包脚本、游戏格式配置或配套参数 | 不等于一个安装器格式 |

文件级清单保留了所有剩余文档、配置和资源，缺乏版本描述的库明确标注为配套库，不臆测其完整接口。

## 自主实现的具体顺序

### 1. 先补 MSI 媒体形式和标准 CAB

当前 `src/formats/msi.cpp` 只接受一条 Media 记录、一个以 `#` 开头的内嵌 CAB。已有 FDI 解压能力，但还没有独立 `.cab` 输入格式。

建议拆成两个交付：

1. 自己解析 MSI 的多条 `Media`、`File.Sequence` 和 `Directory`；支持多个独立内嵌 CAB、同目录外置 CAB。压缩与非压缩文件混合、松散源文件需要按 MSI 属性和源目录规则处理。
2. 增加标准 Microsoft CAB 的格式入口和经过边界验证的 CAB SFX；先支持独立 CAB，再处理跨卷/跨 CAB 文件续接。FDI 回调统一接入文件预算和安全路径策略。

不要只扫描 `MSCF` 四字节就认定找到了归档，须验证 CAB 头、长度和文件范围；缺失外置媒体要明确报告文件名。先完成主包载荷，不模拟 CustomAction。

多媒体映射参考 [Microsoft Media Table](https://learn.microsoft.com/en-us/windows/win32/msi/media-table)；CAB 内嵌流参考 [Including a Cabinet File](https://learn.microsoft.com/en-us/windows/win32/msi/including-a-cabinet-file-in-an-installation)。

同时继续维护现有 Inno/NSIS。`docs/11-common-wrappers.md` 中两例 Electron 包已经提取应用载荷，但因卸载器路径无法静态还原而返回部分完成；这类路径和结果表达改进比添加冷门格式更直接。

### 2. 新增 WiX Burn

这是这批工具中最值得新增的独立封装解析器。`dark.exe` 是可参考的入口；现代 WiX 的 `wix burn extract` 明确区分 Bootstrapper Application 容器与其它容器。

自行解析 PE/Burn 元数据、容器边界和 manifest 中载荷标识到文件名的映射，调用标准 CAB 层展开，再将 MSI/EXE 子包交给现有递归层。不要只解出一堆匿名 CAB 项就报告应用已完整提取。

首批验收覆盖至少两个已确认结构版本、多个附带包、外置容器缺失、在线载荷缺失、截断/损坏容器。BA、前置依赖与主应用载荷分别记录；不执行 Bundle 的规划/安装动作。新旧 WiX 用相应源码与生成样本验证，不能从本地 dark 3.x 推断新版本全兼容。参见 [WiX 命令文档](https://docs.firegiant.com/wix/tools/wixexe/)和 [Burn Container](https://docs.firegiant.com/wix3/xsd/wix/container/)。

### 3. MSIX / APPX 与 Bundle

这两个目录没有找到同名专用工具，不妨碍它成为有价值的新目标。复用现有 ZIP 读取，自己处理 manifest、包标识、架构/资源包和 bundle 成员关系；保留 VFS 目录含义，不能仅去掉扩展名按普通 ZIP 输出就宣称完成 MSIX 支持。

首批做未加密单包和 bundle，保留全部架构并在报告中标注，校验 block map 与载荷；加密包、稀疏包外部位置、缺失依赖明确报告。包完整性校验与证书信任判断分别表述。验收时可用 Windows SDK 的 MakeAppx 作为对照工具，发布程序不调用它。参见 [Microsoft MakeAppx](https://learn.microsoft.com/en-us/windows/msix/package/create-app-package-with-makeappx-tool)。

### 4. Squirrel / Velopack 离线包

两者是不同封装实现，不能只按 Electron 应用名称选择解析器。分别核查 EXE 内嵌载荷位置、完整更新包的归档结构和应用目录映射；按实际结构复用 ZIP 层，必要时先补 PE 资源内载荷定位。

首批完整离线载荷现已在本地实现：Velopack 固定标记和 Squirrel.Windows DATA/131 分别解析。无完整载荷、只有 delta 或缺少基础版本时给出具体原因，不执行更新器和安装钩子。具体固定源码、测试证据与仍未覆盖的情况见 [专项实现记录](24-update-packages.md)。

### 5. InstallShield 与 Advanced Installer：拆成不同任务

| 子任务 | 实现内容 | 优先级和条件 |
| --- | --- | --- |
| InstallShield EXE 外壳 | 解析近期外壳的目录/资源，识别内嵌 MSI、标准 CAB 或专有 CAB/HDR，继续递归 | 第二批，有近期样本即可提前 |
| InstallShield 专有 CAB/HDR | 自己实现文件表、目录/组件/文件组、多卷寻址和完整性检查，底层使用原生解码库 | 值得做；成本高于标准 CAB，先限定已验证结构 |
| Advanced Installer EXE | 先调查内嵌 MSI/CAB 和资源映射，区分在线、外置媒体、内嵌媒体 | 值得立项调查；当前还不能给出通用静态提取承诺 |

**InstallShield CAB 与 Microsoft CAB 是两种格式。** `unshield` 是有价值的数据结构参考，但按照用户已确认的边界，安装器解析逻辑由我们实现，不直接把 libunshield 当成已完成的自研模块。官方 2025 文档仍介绍 InstallScript 的 `data1.cab` / `data1.hdr`，因此不应因 i5comp/i6comp 很旧就排除所有 InstallShield。参见 [InstallShield 2025 文档](https://docs.revenera.com/installshield31helplib/helplibrary/IsCabView-Viewing.htm)和 [unshield 格式检查代码](https://github.com/twogood/unshield/blob/main/lib/helper.c)。

Advanced Installer 官方提供 `/extract`，但本次检查的 UniExtract2 `TYPE_AI` 分支直接 `ShellExecute` 输入安装包；这不符合本项目静态读取的实现路径。该分支能说明它如何调度，**不能代替 EXE 格式解析源码**。参见 [Advanced Installer 官方命令](https://www.advancedinstaller.com/user-guide/exe-setup-file.html)和 [UniExtract2 对应源码](https://github.com/Bioruebe/UniExtract2/blob/9719ac988421e48276420e2f33e09087cfbacf8d/UniExtract.au3#L2385-L2390)。

### 6. 样本驱动的候选与明确后置的范围

| 候选 | 处理策略 |
| --- | --- |
| Setup Factory、CreateInstall/Gentee、Smart Install Maker、Clickteam | 已有具体解析工具可参考；拿近期失败样本后选一个实现，不同时重做全部历史版本 |
| install4j、InstallAnywhere 等 Java 安装器 | 不把名称相近的产品合并。UniExtract2 对 install4j 使用 QuickBMS 脚本；需要 Java 应用样本再做专用解析 |
| InstallAware、其它 ZIP/7z 型 EXE | 先判断现有归档/SFX/递归层能否覆盖，再补外壳或映射。当前 UniExtract2 将 InstallAware 路由到 7z，不表示每个版本都是同一种静态布局 |
| RAR SFX | 如果用户实际下载包经常遇到，可提前到第二批；目前五个本地安装器样本不足以证明其优先级 |
| ISO/UDF | 作为安装包外层载体可选；只有希望直接拖 ISO 时才扩展，不因工具箱有镜像转换器就全做 |
| Enigma Virtual Box | 若希望拆便携单文件程序，再立项；与安装包提取目标区分 |
| 游戏、媒体、邮件、PDF、数据库、固件、ASPack/UPX 脱壳、AutoIt 反编译 | 不纳入当前产品范围 |
| Wise、InstallShield 3、Reflexive、Flash、Eschalon、VISE、ACE/ARJ/KGB/UHARC 等历史专用兼容 | 不投入主线资源；近期真实需求出现后再评估 |

## 实现和验收方式

每新增一种封装都沿用 `识别 → 结构验证 → 文件清单 → 安全路径规划 → 流式提取 → 内部校验 → 嵌套识别 → 报告`。识别只产生候选，结构验证成功才选择解析器。

自己实现：PE 资源/附加区寻址、安装器元数据、目录/条件映射、容器关系、递归策略、完整性结果和诊断。继续复用：原生解压基础库、现有 ZIP/7z、Windows MSI 只读数据库 API/FDI、Win32 文件与通知。

不要复制 UniExtract 的“依次试很多外部命令，发现输出文件就算成功”。测试既检查外层文件，又检查应用实际载荷；必要时与原始打包文件或独立解析器清单比较。静态文件提取完成不等于应用一定可直接运行。

每个模块至少覆盖：多个近期生成样本、中文/空格路径、嵌套应用包、缺失外置载荷、损坏数据、冲突文件名、路径越界和累计解压预算。真实样本从第一版开始保留，测试记录精确结构版本和最终文件哈希。

2026-09-07 已取消 512 MiB 输入及共享 8 GiB/10,000 文件预算，改为实际磁盘空间和整数范围检查。v0.7.0 发布后本地增加 [worker/可选超时](22-worker-cancellation.md)；现有连续映射及解码器约束仍影响大包覆盖，详见 [资源处理](17-large-packages.md)。

## 核查依据

- 本地 `A/../docs/FORMATS.md`、`docs/third-party/*` 作者说明，以及 `A/../def/*.ini`：辅助判断具体工具用途，不把历史格式表当作本地运行验证。
- [UniExtract2 工具名称与来源表](https://github.com/Bioruebe/UniExtract2/blob/9719ac988421e48276420e2f33e09087cfbacf8d/docs/helper_binaries_info.txt)：帮助定位作者和项目；表中版本不代表本地版本。
- [UniExtract2 源码](https://github.com/Bioruebe/UniExtract2/blob/9719ac988421e48276420e2f33e09087cfbacf8d/UniExtract.au3)：实际调度分支；`TYPE_THINSTALL` 在约 3107 行启动目标进程后提取。
- [原版 Universal Extractor 作者说明](https://www.legroom.net/software/uniextract)：历史工具角色与格式背景。
- [lessmsi](https://github.com/activescott/lessmsi)、[unshield](https://github.com/twogood/unshield)：分别对应 MSI 和 InstallShield 专有 CAB。
- 当前项目 `README.md`、`src/formats/msi.cpp` 和 `docs/11-common-wrappers.md`：判断已有能力与缺口。现有测试结果引用项目报告，本轮未重新执行构建或安装器测试。
