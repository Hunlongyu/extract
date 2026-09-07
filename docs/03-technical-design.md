# 技术设计

日期：2026-09-07。0.5.0 已实现 MSI 多媒体、标准 CAB、Burn 首批结构、选定 Inno/NSIS 结构、ZIP/7z、递归展开、统一输出和系统通知；工作进程与硬超时仍为后续设计。最新模块与边界见 [MSI/CAB/Burn 实现](13-msi-cab-burn.md)及 README。

## 1. 结构

```text
Extract.exe：Win32 参数入口 / 任务控制 / 系统通知
    └─ 当前顺序执行；后续隔离到相同 EXE 的内部 worker 模式
        └─ extract_core：本项目 C++20 核心
            ├─ MSI / Inno / NSIS / Burn 安装包处理器
            ├─ MSIX / Velopack 等后续处理器
            ├─ 通用归档与压缩基础库
            └─ 有边界的输入流 / 文件计划 / 校验 / 安全写入
```

安装包解析逻辑编入自己的核心库。首版在主进程顺序执行；规划中的 worker 使用本项目进程隔离解析崩溃并实现硬超时，不调用第三方解包程序，不建立通用插件 ABI、常驻服务或数据库。

自研主程序和解析模块统一使用 C++20，允许底层 C 基础库；工程默认 C 标准为 C17，第三方源码需要不同标准时在其目标单独设置。不为混合语言而刻意拆分自研模块。Win32 程序可以使用 Windows 自带的 COM / WinRT 通知接口，无需 .NET、AutoIt、Qt 或 Windows App SDK。

## 2. 模块建议

```text
src/
  app/            main、参数、批处理、worker 管理
  core/           检测结果、文件计划、执行状态、预算
  io/             有界读取、切片、路径校验、原子提交
  formats/
    pe/           PE32/PE32+ 与资源/附加数据定位
    msi/          表读取、媒体和目录映射
    inno/         版本布局、头部、文件表、数据块
    nsis/         头部、字符串、指令与文件数据
    archive/      ZIP/7z/CAB 基础适配
    msix/         后续清单与 Bundle
    burn/         后续容器与载荷映射
    velopack/     后续离线包结构
  codecs/         zlib、LZMA SDK、bzip2、FDI 适配
  platform/       Win32 文件/句柄、通知、系统数据库 API
resources/        manifest、rc、图标
third_party/      原生基础库及许可证
tests/            单元、格式样本、损坏输入、集成验证
```

发布目录保留 `Extract.exe`、必要原生库、使用说明、第三方许可和构建版本清单。格式模块初期静态链接；更新自己的程序和相关原生库即可更新解析能力。没有 `engines/lessmsi` 等外部工具目录。

## 3. 统一处理接口

每个处理器提供以下内部能力，具体 C++ 类型在实现时确定：

| 操作 | 输出 |
| --- | --- |
| `probe` | 格式、结构版本、检测依据、确认/可能/不匹配 |
| `read_catalog` | 条目 ID、目标路径表达式、数据位置、大小、校验、条件、子包线索 |
| `make_plan` | 本地相对输出路径、冲突处理、所需媒体、未解析项和预算 |
| `extract` | 通过统一输出接口写文件并报告进度 |
| `verify` | 条目完成情况、包内校验结果和未满足条件 |

自有结果类型使用枚举、结构体与 `std::variant` 等 C++20 能力，不依赖 C++23 的 `std::expected`。处理器不能自己弹窗、执行安装器、访问网络或随意拼接磁盘路径。

## 4. 输入、检测与版本管理

使用宽字符入口读取 Explorer 传入的路径。以文件句柄和有限长度的切片提供输入，拒绝读取越界、整数溢出与不合理的计数/字典大小。保持输入句柄在探测和提取间一致，若下层 API 必须按路径重新打开，则锁定/快照输入并核验文件身份，不能静默读取被替换的包。

PE 定位器解析 PE32 与 PE32+，检查节和目录边界。不能仅用“最后一个节后面的所有数据”认定安装载荷：签名证书、overlay、资源和格式指定偏移都需要辨别。文件扩展名只作为提示。

格式识别返回证据；强识别后按该处理器报告错误。只有不匹配才尝试其它处理器，损坏/加密/缺卷不能被后续普通 PE 提取误判成功。

使用按结构版本分组的布局描述和能力表。产品版本与实际数据版本分开记录，未知的新结构应明确返回“不支持该结构”；不能盲目套用最接近的旧布局。生产方稳定 tag 与样本共同决定兼容分支。

## 5. MSI 模块

MSI 的本项目实现分为“读取系统数据库记录”和“解析安装包语义”两层。通过 `MsiOpenDatabaseW(..., MSIDBOPEN_READONLY, ...)`、查询与流读取 API 获得数据；本项目完成关联、文件计划及提取。只读数据库模式由系统提供，不调用任何安装动作。[API 模式说明](https://learn.microsoft.com/en-us/windows/win32/api/msiquery/nf-msiquery-msiopendatabasea)

0.5.0 已实现内嵌/外置的多个独立 CAB、松散文件与混合源。跨卷续接、管理映像、MST/MSP 和安装动作仍不支持。处理流程：

1. 读取 File、Component、Directory、Media、Summary Information，必要时读取 MsiFileHash。
2. File 关联 Component，再关联 Directory；解析短名/长名、源/目标目录规则与根标识，构建输出树。[File 表](https://learn.microsoft.com/en-us/windows/win32/msi/file-table)、[Directory 表](https://learn.microsoft.com/en-us/windows/win32/msi/directory-table)
3. 根据文件压缩标志与摘要中的默认压缩状态区分 CAB 和松散文件。
4. 结合 Sequence 与 Media.LastSequence 定位媒体；`#` 开头的 Cabinet 从内部流读取，其余从允许的同目录媒体集合读取。[Media 表](https://learn.microsoft.com/en-us/windows/win32/msi/media-table)
5. 自行解析 CAB 文件表并按成员标识关联到 File 条目，FDI 受控回调只写入规划后的路径；松散文件按 source 布局和长短源名称标志读取。跨卷文件拒绝，缺失本地来源给出具体文件名。
6. 比较预期清单、大小和可用校验；保存目录及条件映射，不执行 CostFinalize、CustomAction、脚本或 `msiexec /a`。

参考入口：[lessmsi 的 Wixtracts.cs](https://github.com/activescott/lessmsi/blob/master/src/LessMsi.Core/Msi/Wixtracts.cs)。参考其数据关系，用 C++ 实现模块，不把 C# 程序作为运行依赖。

当前提取 File 表静态描述的源文件，不模拟安装选择条件、DuplicateFiles 动作、补丁或 MST 变换，也不导出 Binary 辅助流。空 File 表的配置型 MSI 输出零文件清单和说明。系统目录属性映射到输出树里的逻辑目录，未知属性保留标识。

共享 CAB 层还服务于独立 `.cab`、PE 附加区 CAB 与 Burn；校验标准 Microsoft CAB 1.3 的头部、文件表、计数、编码和数据边界。多个独立 CAB 与跨卷续接是不同能力，InstallShield 私有 CAB 也不是此格式。

FDI 的部分接口使用窄字符。适配层用受控 ASCII 标识映射到真实宽字符句柄/流，避免将中文源路径直接损失转换；跨卷回调也必须走同一映射。[FDICopy](https://learn.microsoft.com/en-us/windows/win32/api/fdi/nf-fdi-fdicopy)

## 6. Inno 模块

0.2.0 已实现精确数据版本 6.5.2、6.6.1、6.7.0、7.0.0.3 的内嵌静态载荷；五种压缩与 solid 已通过生产方生成的已知文件对照。当前源码为 `formats/pe.cpp`、`formats/inno.cpp`、`codecs/stream.cpp`。外置卷、加密和其它结构仍按以下总体设计继续开发。

主要格式依据为生产方稳定版的结构定义和读取流程；innoextract 用作另一种实现的对照。先核对当前 6.7 与 7.1，再按样本回补 6.2–6.6，目标不是把所有历史版本分支搬进来。

1. 定位 loader 偏移表/数据标识，验证元数据区长度和校验。
2. 按数据结构版本解析头部、字符串及记录表；即便不导出 UI/脚本表，也必须正确跳过它们，才能读到后续文件数据。
3. 建立文件条目、数据块、压缩方法、solid 流和外置卷的对应关系。
4. 将原始或压缩数据流交给基础解码器，按条目偏移输出；校验块和文件信息。
5. 保留逻辑目录和条件，不执行 Pascal 脚本；相同目标路径的不同文件作为独立变体保存。

源码研究入口：[Inno Setup](https://github.com/jrsoftware/issrc)、[innoextract](https://github.com/dscharrer/innoextract)。结构表需固定到已发布 tag，不能从更新中的主分支直接推断历史版本。

Inno 7 加入 64 位安装器和扩展长度路径，且允许更大的 LZMA 字典。因此同时验证 32/64 位 loader、数据布局、长路径与内存上限；不能把 native 指针宽度当磁盘字段宽度。[Inno 7 变更说明](https://jrsoftware.org/files/is7-whatsnew.htm)

只把检测到的加密/密码需求报告为对应状态，不把“解码失败”一律归因于版本。6.x 的产品补丁版本不一定改变数据布局，需要比较生产方结构与样本确认。

## 7. NSIS 与 Electron 模块

目标是近期 NSIS 3.x Unicode 安装包，同时覆盖真实 Electron-builder 使用的变体。主线 NSIS 支持不能自动等同于 Electron/NSIS 支持。

0.3.0 已实现本节第 1–5 项的受限版本；0.3.1 增加安装包递归展开和 Inno 6.5.0。0.4.0 增加 ZIP/7z/SFX 与 Electron 的 `app-64.7z` 载荷提取。NSIS 目录通过显式赋值和控制流合流传播，分支值不一致、动态赋值或内部函数改变目录时保守回退；完整 StrCpy 另保留基本块内传播。插件任意修改变量的副作用不模拟。Solid 缓存受目录锁保护，CRC 与逐文件摘要分别记录。真实样本和限制见 [常见封装验收](11-common-wrappers.md)。

1. 解析 first header、块索引、压缩头与固实/非固实数据区。
2. 适配安装器的 Deflate、bzip2、LZMA 流细节；不能假设和普通 ZIP/独立 bzip2 文件包装一致。
3. 读取字符串、目录设定和文件提取相关指令，构建静态文件映射。
4. 对分支做受限静态分析：能证明的目标路径写入对应逻辑目录；依赖注册表、运行参数、任意脚本计算的路径保留为 unresolved。
5. 不执行插件/指令；不能解析的动态行为记录为限制。提取出的插件 DLL 是数据，不加载执行。
6. 对验证过的 Electron 封装规则识别内层应用归档，再用通用归档基础层展开。多架构载荷分别保留，不默认只取当前电脑架构。

参考：[NSIS 源码入口](https://nsis.sourceforge.io/Download)、[NSIS 解析实现](https://github.com/ip7z/7zip/blob/main/CPP/7zip/Archive/Nsis/NsisIn.cpp)、[Electron-builder NSIS 文档](https://www.electron.build/nsis/)。

`nsis-web` 可能只有下载逻辑，没有本地应用载荷；不因为读取到下载器文件就显示应用已完整提取。识别不到具体下载行为时使用“不支持/无法确认”，不武断断言是在线安装器。

## 8. 现代封装扩展

### MSIX / APPX

读取通用 ZIP 容器，自己的模块解析 AppxManifest、AppxBlockMap 与 Bundle 清单，核对块哈希并展开受支持的内嵌包。XML 禁用外部实体。包架构、资源与依赖保持可追溯，不执行部署。加密或需要外部文件的包返回明确状态。[MSIX 结构](https://learn.microsoft.com/en-us/windows/msix/overview)

### WiX Burn

自己的模块定位 bundle 元数据、附加容器和载荷，恢复载荷名称；已有 MSI/Inno/NSIS 处理器再处理内层。按结构分别覆盖近期版本，不能只识别产品名。分离的外置容器只能从允许媒体位置读取；远程载荷只记录缺失。

0.5.0 实现 `.wixburn` 布局版本 2 和 CAB 容器，分别计算 UX、原始引擎签名、附加容器与最终 PE 证书的边界。XmlLite 禁止 DTD，解析 v3/v4 命名空间；清单 `SourcePath` 关联 CAB 内部编号，`FilePath` 规划逻辑输出，容器和载荷分别验证 SHA-1/256/512。缺失外置来源设置 `contentComplete=false`；实际存在但损坏的来源失败并回滚本层。详情见 [本轮实现记录](13-msi-cab-burn.md)。

参考生产方提取实现与容器代码，不能运行 bundle 的 `/layout` 或其它安装参数作为解析替代。[Burn 文档](https://docs.firegiant.com/wix/tools/burn/)、[提取工具说明](https://docs.firegiant.com/wix/tools/wixexe/)

### Velopack / Squirrel

分别研究近期版本的 Windows 启动器与包布局，定位离线完整载荷，再交给通用归档层。基于完整包实现文件输出；差分包要求精确基线，否则提示缺少基线，不能把补丁文件当应用程序。[Velopack](https://github.com/velopack/velopack)、[Squirrel.Windows](https://github.com/Squirrel/Squirrel.Windows)

## 9. 统一落盘与嵌套限制

所有格式先生成文件计划，再通过同一个写入器创建文件。该写入器拒绝绝对路径、越界 `..`、ADS、设备名和不支持的链接，检查大小写与规范化冲突。保留原始路径和条目 ID；对有效的条件变体使用独立目录，对危险路径拒绝写入。

输出根和父目录需要检查 reparse point，创建文件时避免跟随被替换的目录链接；仅做字符串前缀比较不足以判定落盘范围。尽可能保持输入与输出句柄控制，不使用一次性检查后无限信任路径的方式。

嵌套处理由 `core/tree.cpp` 实现：0.5.0 最多 4 层、32 个包（均含根层），各层累计 8 GiB / 10000 个文件。包数上限从 16 扩至 32，以容纳 Burn 的多组件安装链；其余累计上限不变。原始内层包计入预算，按 SHA-256 检查活动链；MSI/Burn 因外置来源可能不同，不复用只按输入包摘要缓存的结果。每层提交后释放输入和 Solid 缓存，再处理子包。打开子包后复核摘要，防止层间替换。所有原始内层包保留，报告保存父子关系、累计输出和子状态；内层失败使祖先报告 partial，不删除已经成功提交的外层。预算目前是固定值，配置化、硬超时及进程内存限制仍待实现。

完成后同卷提交结果目录；目录名被并发占用则换序号，不覆盖。部分结果单独标记，失败清理只处理本任务拥有的临时目录。散列与块校验的可用性分别记录，不能用文件数一致代替数据完整性。

## 10. 工作进程、结果与通知

0.5.1 将原始路径还原状态与完成条件分开：`Entry.path_resolved` 保留逐文件事实，已按结构识别的卸载器记为 `is_uninstaller`。`Catalog.required_paths_resolved()` 仅排除这些卸载器的路径问题；内容完整性与子包状态仍独立参与最终判定。识别使用已提取文件的包头，且在输入句柄保护下复核上层记录的摘要。报告保留 `pathsResolved=false`，但允许卸载辅助文件之外的路径均已还原时 `requiredPathsResolved=true`。没有逐文件原因的处理器继续保守地保留未完成状态。

最终目录选择在辅助文件分类后进行；仅有卸载器的 app 目录不会盖过已完成的内层应用目录。详情见 [卸载器与完成状态](14-uninstaller-completion.md)。

父进程使用 `CreateProcessW` 启动自己的内部 worker 模式，以显式句柄传入任务；worker 只读取允许的输入和写入计划内输出。内部协议有版本与长度检查，普通文件参数不能被当成协议命令。

worker 纳入 Job Object，父进程关闭或超时后回收；Job 用于生命周期和资源控制，不能作为文件系统沙箱。解析、校验和日志在 worker 内完成，父进程负责记录结局和发送通知。基础库全程通过流/句柄接口使用。

统一状态包含 Complete、OuterOnly、Partial、Unsupported、ExternalRequired、Corrupt、IoError、Timeout、InternalError。进程退出码按批次聚合，详细状态保存在记录中；通知失败不会改变已经正确完成的解包结果。

系统通知已通过 Windows COM/WinRT 与 WRL 实现。当前采用 AUMID、占位 CLSID 快捷方式和当前用户 URI 协议注册；没有 COM LocalServer32。`hunlongyu-extract://job/<GUID>` 只允许固定任务标识，激活后打开任务记录中的目录；文本经过 XML 转义。程序退出后由系统启动新的 Extract 进程。[微软 C++ 通知接口示例](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/DesktopToasts/CPP/DesktopToastsSample.cpp)

## 11. 构建与依赖

CMake 当前包含 `Extract`、`extract_core`、路径测试、日志探针与各格式集成测试；通知激活探针单独手动构建，避免普通测试修改用户注册。自有 C++ 固定 C++20，默认 C 标准为 C17；依赖库独立设编译选项，避免把第三方告警策略强加到整个项目。0.6.0 的详细日志模块随 `extract_core` 编译，目录回退、大小轮转和保留策略见 [日志实现](15-logging.md)。

MSVC 使用 `vswhere.exe -prerelease` 或 Insiders 的 `vcvars64.bat` 发现/初始化环境，不固定 MSVC 版本子目录。主程序为 Windows 子系统，Unicode、`asInvoker` 与长路径清单；启用基本编译和链接保护。

初始化阶段提供 Ninja 的 x64 Debug/Release 预设、PowerShell 构建脚本和安装目录规则。默认静态链接 MSVC CRT；实际运行依赖在发布时检查，不通过关闭安全检查压缩体积。操作步骤见 [构建与开发](05-build-and-development.md)。

按使用情况链接系统的 `msi`、`cabinet`、`bcrypt`、`xmllite`、`shell32`、`ole32`、`propsys`、`runtimeobject` 等库。所有第三方基础库固定来源、版本、摘要和许可；构建时不追随浮动最新分支。

格式代码、基础库版本、支持能力和样本报告随发布记录，便于定位“同一个包为什么某个版本能解、另一个版本不能解”。
