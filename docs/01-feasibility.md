# 可行性与技术选型

日期：2026-09-05。本文件记录初始可行性分析与技术选型；已实现 MSI 内嵌单 CAB 和选定近期 Inno 结构，实际范围见 [MSI 报告](06-msi-first-slice.md)与 [Inno 报告](07-inno-implementation.md)。其余格式仍为规划目标。

## 1. 结论与实现边界

**可行，推荐做一个面向近期安装包的原生解包器。** 开源参考能提供格式识别、数据组织和目录还原的经验；本项目用新的技术栈实现安装包处理逻辑，底层压缩算法使用成熟原生库。

| 层次 | 实现责任 |
| --- | --- |
| 拖拽入口、任务管理、日志、系统通知 | 本项目 C++20 / Win32 |
| 安装器识别、版本分支、文件索引、目录映射、嵌套包规则 | 本项目 C++20 |
| 有边界的二进制读取、流适配、文件落盘 | 本项目 C++20 |
| Deflate、LZMA/LZMA2、bzip2 等算法 | 允许使用原生 C/C++ 库 |
| 通用 ZIP/7z 容器基础读取 | 使用小型原生基础库或 SDK 子集，安装器语义仍由本项目处理 |
| MSI 数据库记录读取、CAB 解码、系统加密与文件 API | 可用 Windows 系统 API，安装布局与提取决策由本项目实现 |

发行版不运行 lessmsi、innoextract、UniExtract 或 7z 等外部解包程序，不引入它们的 .NET / AutoIt 运行依赖；也不以链接一个完整现成 Inno/NSIS 处理器替代本项目的安装包解析模块。解析模块初期编入自己的核心库，无需动态插件系统。

这项工作比简单调用 CLI 多了协议实现和版本测试，但不需要从头编写所有压缩算法，也不需要实现安装脚本执行器。

## 2. “近期安装包”的定义

推荐以 2021–2026 年发布的软件安装包为第一批样本，侧重最近两年的版本和当前稳定发行包。时间窗口用于安排开发和验收，不是运行时的时间戳检查。

必须区分“旧格式”与“旧安装器”：MSI、ZIP、CAB 历史很长，仍可用于新软件；它们不能因为出现早就被排除。相反，Wise、早期 InstallShield CAB、NSIS 1/2、Inno 5 及更早专有结构，不列入主动适配和验收范围。旧包若恰好能走当前路径，可正常提取，但不为其增加专属分支。

近期 InstallShield / Advanced Installer 生成的标准 MSI 由 MSI 模块处理；它们的 EXE 引导器需要单独识别，不能把整个产品家族判成老旧或已支持。

## 3. 当前格式信息与目标

本次官方页面核验：Inno Setup 同时提供 6.7.3（2026-05-26）和 7.1.0（2026-08-12），NSIS 下载页列出 3.12（2026-04-19）。这些是调研日的版本记录，实施前应再次核对，且不能把开发分支内容当成稳定版。[Inno 下载页](https://jrsoftware.org/isdl.php)、[NSIS 下载页](https://nsis.sourceforge.io/Download)

| 格式或封装 | 优先级 | 计划范围 |
| --- | --- | --- |
| MSI | P0 | 近期发行的标准 MSI，内嵌/外置 CAB、同目录松散源文件，文件名与逻辑目录还原 |
| Inno Setup | P0 | 6.2–6.7 与 7.0/7.1 结构族；先验证当前 6.7 和 7.1，再回补窗口内差异 |
| NSIS | P0 | 3.x Unicode 主线，常见压缩与 solid 模式；近期修改版单独验证 |
| Electron/NSIS | P0 | 主流离线安装包中的多架构和嵌套归档，不能停在外层解出一个大文件 |
| ZIP / 7z / Microsoft CAB | P0 基础能力 | 为安装器载荷服务，同时支持直接输入；不追求独立压缩软件的所有功能 |
| MSIX / APPX 与 Bundle | P1 高优先级 | 包内容、清单、块校验与内嵌子包，不执行部署 |
| WiX Burn | P1 高优先级 | 近期 bundle 的内嵌容器、载荷名称映射及 MSI/EXE 子包；按结构版本逐个验收 |
| Velopack / Squirrel 系列 | P1 | 近期软件的离线安装载荷、完整更新包；两个家族分别研究，不能假设结构相同 |
| 当前其他 EXE 引导器 | 样本驱动 | 检查是否内嵌已支持的容器；有近期使用价值才增加处理器 |
| 历史专有安装器、游戏封包、磁盘/文件系统镜像、通用脱壳 | 不在目标内 | 避免为数量扩展冷门兼容 |

Electron-builder 的 Windows 默认目标是 NSIS，文档还描述了离线包与下载型 `nsis-web` 的区别。因此 Electron/NSIS 值得优先安排，但不能把 Electron 应用都当成同一种安装包。[electron-builder](https://www.electron.build/nsis/)

MSIX 是单独的部署包体系；Burn 是包含安装包链和引导程序的封装；Velopack 则是安装与更新框架。它们应该按各自内容结构处理，而不是统一套用 EXE 资源提取。[MSIX](https://learn.microsoft.com/en-us/windows/msix/overview)、[Burn](https://docs.firegiant.com/wix/tools/burn/)、[Velopack](https://github.com/velopack/velopack)

## 4. 参考项目如何使用

| 来源 | 参考价值 | 本项目如何落地 |
| --- | --- | --- |
| [Bioruebe/UniExtract2](https://github.com/Bioruebe/UniExtract2) | 格式分类、探测顺序、失败分支、样本线索 | 编写自己的检测与分发模块，只研究近期目标格式 |
| [lcorbasson/uniextract](https://github.com/lcorbasson/uniextract) | 原版源码镜像，理解早期调度设计 | 辅助参考，不作为现代格式支持基线 |
| [activescott/lessmsi](https://github.com/activescott/lessmsi) | MSI 表关联、CAB 与文件映射、目录重建 | 用 C++ 和 Windows API 实现相应流程，参考测试思路 |
| [InnoExtractor 产品页](https://www.havysoft.cl/innoextractor.html) | 提取行为和内容展示能力 | 仅作功能参考，本次未获得其开源解析实现 |
| [Inno Setup 源码](https://github.com/jrsoftware/issrc) | 生产方的数据结构、压缩流和版本变化 | 作为 Inno 新版本布局的主要依据 |
| [innoextract 源码](https://github.com/dscharrer/innoextract) | C++ 解码、版本适配、文件数据关系 | 参考思路并验证差异，不将其旧兼容列表当作我们的上限 |
| [NSIS 解析参考](https://github.com/ip7z/7zip/blob/main/CPP/7zip/Archive/Nsis/NsisIn.cpp) | NSIS 索引、压缩与字符串处理 | 对照 NSIS 源码，编写目标结构的 C++ 处理器 |

UniExtract 前端的“调用哪个工具”信息不足以实现格式解析，需要继续查看打包器本身和专用解析器。InnoExtractor 与 innoextract 是不同项目，产品功能不能混用为源码证据。

## 5. 基础库与系统 API

| 基础能力 | 推荐候选 | 本项目保留的责任 |
| --- | --- | --- |
| Deflate | zlib | 输入边界、ZIP/安装器元数据、输出上限与校验 |
| LZMA / LZMA2、通用 7z 读取 | LZMA SDK 所需 C/C++ 子集 | 安装器特有流包装、索引和目录语义 |
| bzip2 | libbz2 | 容器规则、NSIS 特有压缩差异适配 |
| Microsoft CAB | Windows Cabinet FDI | 媒体选择、成员映射、路径校验与落盘 |
| MSI 记录 | `MsiOpenDatabaseW` 只读及查询 API | 表的关系解析、目录规划、文件提取；不使用安装动作 |
| XML / 哈希 | XmlLite、Windows CNG 等系统接口 | 清单语义、禁用外部实体、完整性规则 |

依据：[zlib](https://zlib.net/)、[LZMA SDK](https://www.7-zip.org/sdk.html)、[bzip2](https://sourceware.org/bzip2/)、[FDICopy](https://learn.microsoft.com/en-us/windows/win32/api/fdi/nf-fdi-fdicopy)、[MSI 数据访问](https://learn.microsoft.com/en-us/windows/win32/api/msiquery/nf-msiquery-msiopendatabasea)。

算法相同不表示流可以直接互换：某些安装器会修改块头、校验或流包装，必须由安装器模块适配。基础库不准自行选用绝对路径写文件，输出必须经过本项目的统一写入接口。

## 6. 原生实现中的实际难点

- **Inno 新版本**：定位元数据、读出精确结构、跳过非文件表、关联压缩块和文件、验证校验值。7.x 的 32/64 位安装器和大字典必须进入测试；不能按运行平台直接映射磁盘结构。[Inno 7 变更](https://jrsoftware.org/files/is7-whatsnew.htm)
- **NSIS**：文件目标路径与脚本指令有关。能确定的路径做静态解析，依赖运行环境或插件生成的路径保留为未解析项，不执行脚本猜结果。
- **MSI**：参考公开表规则与 lessmsi，自行完成 File → Component → Directory、Media 和 CAB 成员关系；系统 API 降低存储格式读取成本。
- **嵌套载荷**：近期安装器常有外层引导包和内层应用包。需要可追溯地处理已知层级，保留外层原始文件，限制递归。
- **结果含义**：应区分静态载荷完整提取、仅外层提取、部分提取、需要外部下载或基线包，不统一显示成功。

## 7. 支持范围的限制

没有嵌入到包里的在线下载内容，不能仅靠离线解包生成；差分更新缺少对应基线时，也不能还原完整应用。首版提取静态文件，不执行注册、驱动安装、脚本或自定义动作，不保证提取出来的软件可以直接运行。

密码保护先返回明确原因，后续可加入用户提供密码的原生输入流程；不破解密码。未知近期结构应记录为待适配，不能因为未实现就误报文件损坏。

## 8. 源码与许可证

参考思路和移植源码要分别记录。自行编写的模块记录格式依据；如移植具体实现，保留来源、版本、修改说明并核对其许可证，不能把语言翻译当成自动免除源代码许可义务。基础库按选定版本保存许可和必要分发材料。

重点区分 LZMA SDK 子集与完整 7-Zip 安装器解析代码：两者不是同一许可范围。前者官方说明为公有领域，完整 7-Zip 则包含 LGPL 等条款。[LZMA SDK](https://www.7-zip.org/sdk.html)、[7-Zip 许可](https://www.7-zip.org/license.txt)

本次未复制第三方程序或实现代码，未安装任何解包引擎。
