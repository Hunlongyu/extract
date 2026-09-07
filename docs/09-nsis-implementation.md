# NSIS 外层解包实现与验证

本文件记录 0.3.1 阶段。0.4.0 的自定义品牌文本、目录控制流与 ZIP/7z 载荷支持见 [常见封装验收](11-common-wrappers.md)。

日期：2026-09-05。版本：0.3.1。已交付 NSIS 包内静态文件提取、已识别内嵌安装包的自动展开及 XnView 两层内容验证。Electron 最终应用载荷尚未验收。

## 1. 已实现范围

| 项目 | 行为 |
| --- | --- |
| 识别 | 检查 PE 节范围，在外层附加区的 512 字节边界查找 NSIS first header |
| 索引 | 自行读取块表、File 指令、Unicode 编码字符串与语言表 |
| 压缩 | stored、raw Deflate、NSIS bzip2、LZMA；逐块与 Solid |
| 路径 | 基本块内传播完整 StrCpy、SetOutPath；目录变量保留为逻辑根 |
| 分支 | 保留各分支 File 内容，不执行条件；跨分支/调用不推测变量值 |
| 冲突 | 同路径、同块的重复指令合并并记录指令编号；不同内容、大小写或前缀冲突进入 `_variants` |
| 校验 | 验证包中存在的 CRC32；提取后逐文件计算 SHA-256 |
| 完成状态 | 静态路径及已识别内层均完成才报告 complete；动态路径、内层不支持/失败/超限返回 299 |
| 输出 | 复用安全输出、失败清理、同名防覆盖、报告与静音系统通知 |

自研代码使用 C++20；基础解码库按 C17 编译。产品不调用 makensis、7z、UniExtract 或其它解包程序。

## 2. 结构与实现

生产方布局固定为 [NSIS v312](https://github.com/NSIS-Dev/nsis/tree/v312)，提交 `e3f60402bcdf7be822d159b531c6e38ddf32de12`。研究依据是 `Source/exehead/fileform.h`、`fileform.c`、`exec.c`、`util.c` 与 `Source/fileform.cpp`，未引入这些文件的解析器/解释器代码。

### 包头与数据块

first header 共 28 字节，包含标志、`0xDEADBEEF + NullsoftInst`、展开后头长度与本层包长度。标准 CRC 范围从 EXE 的偏移 512 开始，到声明包尾的 CRC 字段之前。输入末尾附加签名不应改变该范围；CRC 不是文件来源认证。

非 Solid 模式每块有 32 位长度，高位表示压缩。File 指令中的数据偏移相对于头部数据块之后的区域；解析器先枚举真实块边界，再允许 File 引用，不能将任意文件中间位置当成新块。压缩方法通过受限候选解码与结构检查判定，不能凭文件扩展名选择。

Solid 模式将头与文件块共同压缩。为生成准确清单，`--list` 也要展开这些块。实现使用 `%TEMP%` 下随机、独立、受目录锁保护的缓存，不再固定限制展开字节量，流式写盘并检查磁盘空间及 64 位累计大小；通过保留原始读写句柄阻止其它写入或删除，只建立只读映射。无论正常结束还是解析失败，都按本任务创建清单清理缓存，不递归遍历未知目录。

### 路径与脚本边界

| NSIS 目录 | 输出逻辑根 |
| --- | --- |
| `$INSTDIR` | `app` |
| `$PLUGINSDIR` | `plugins` |
| `$TEMP` | `temp` |
| `$EXEDIR` | `exedir` |
| Shell 目录编码 | `SHELL_<编码值>` |

这些目录代表包的逻辑空间，不计算用户机器上真实的安装地址或随机插件目录。字符串中的变量、语言引用和转义按编码读取；语言之间不一致时不任选一种路径。完整 StrCpy 可在同一基本块内传播；截取、注册表读取、跨分支/函数调用或其它动态值保留为未知。路径含穿越、ADS、设备名或绝对盘符时拒绝写入。

`sourceExpression` 记录解码与有限符号传播后的表达式，`conditions` 记录 File 指令编号及未执行运行时条件的事实，不是完整脚本反编译结果。没有重建控制流图上的变量合并，也没有解释 NSIS 脚本。即使某些内容在安装时会被删除、覆盖或按条件跳过，静态提取仍保留其包内文件。

不执行插件、不安装或注册文件、不产生脚本 WriteUninstaller/FileWrite/下载逻辑生成的内容。原始内嵌文件作为数据保留；对于 EXE/MSI 中已识别的安装器继续静态展开。`complete` 表示本层清单及已识别内层均完成，不保证软件可以便携运行。NSIS 非应用目录中的 ZIP/7z 载荷目前记录 unsupported，并使任务 partial；未知封装和在线下载不在覆盖范围内。

### 基础库

Deflate 复用 zlib 1.3.2 的 raw 模式，LZMA 复用 LZMA SDK 26.03。NSIS bzip2 与普通 bzip2 不兼容，单独编译 NSIS 的基础 C 解码模块，通过窄 C 接口与标准 bzip2 隔离。此模块仅处理字节流，不读取安装器头、不选择文件路径。

本地适配包括独立配置、符号前缀、selector 数组边界、Huffman 索引实际范围，以及游程整数/长度检查；修改点均标记。来源、版本与署名详见 [基础库管理](../third_party/README.md)。

## 3. 校验与资源限制

报告新增 `packageCrcPresent`、`packageCrcVerified`。存在 CRC 时，损坏即拒绝；明确关闭 CRC 的包可以提取，但这两个字段均为 false，并记录原因。NSIS 通常不带逐文件摘要，因此不能将提取后算出的 SHA-256 当作包内摘要：`sourceHashAlgorithm` 为空，`sourceHashVerified` 为 false。

当前不再限制统一的输入大小、文件总数或输出总量；NSIS 元数据 64 MiB、指令 100,000 条、语言表 256 个、字符串引用深度 16，字符串解析工作量另设上限，LZMA 字典 256 MiB。PE 外壳原始节末尾限制 16 MiB，在其后 64 KiB 的对齐区间定位本层 NSIS。

Solid 缓存和最终输出会同时占用磁盘；进程隔离、硬超时、崩溃后的持久缓存清扫与全进程内存上限尚未实现。

## 4. 自动测试

测试编译器为官方 NSIS **3.11**，ZIP 大小 2,361,546 字节。由于本次 SourceForge 下载未得到二进制文件，改从 Tauri 工具镜像取得原始 ZIP，并与 NSIS 官方发行数据核对 SHA-256：

```text
c7d27f780ddb6cffb4730138cd1591e841f4b7edb155856901cdf5f214394fa1
```

`tests/nsis_integration.py` 只运行官方编译器和 Extract，不执行生成包或解包结果。覆盖：

- 三种压缩 × 普通/Solid，共 6 个矩阵包，逐文件比对打包前内容。
- 不压缩、3 MiB LZMA 字典、关闭 CRC、空文件、中文、Unicode 内容、大于 900 KiB 的 bzip2 分块载荷与不可压缩内容。
- 内容别名、同名不同数据、文件/目录冲突、重复 File 指令、StrCpy、明确分支与动态/合流路径。
- 跳过生成卸载器、声明包长度后附加数据，以及混合成功/失败批次。
- 25 个明确异常样本：CRC、截断、元数据/指令数量上限、无效指令/偏移/字符串/语言引用、危险路径、ANSI 和 bzip2 选择器/组数异常。
- 24 个重算 CRC 的压缩位变异：要求有受控返回结果，不要求任意变异都被拒绝，因为变异后仍可能是有效数据。
- 单独的临时目录验证正常/异常流程结束后无 Solid 缓存残留。

每轮 NSIS 共 82 次 Extract 调用。CTest 包含路径、MSI、Inno、NSIS 和嵌套提取共 5 项；NSIS 单轮记录保存于对应 `nsis-fixtures/<编号>/validation.json`。路径测试额外核验可读缓存句柄拒绝其它写入和删除打开。嵌套测试及最新验收见 [内嵌包展开](10-nested-extraction.md)。

已验证 NSIS 编译器的 **x86 Unicode 安装器**，它可以打包 x64 应用。代码含 PE32+ 字段读取与 8192 字符 section 布局，但尚无对应官方生成样本验收；不据此承诺所有 NSIS 架构或改版兼容。

复现：

```powershell
.\scripts\build.ps1 -Configuration Debug -Test -NsisCompiler 'D:\Tools\NSIS\Bin\makensis.exe'
.\scripts\build.ps1 -Configuration Release -Test -Install -NsisCompiler 'D:\Tools\NSIS\Bin\makensis.exe'
```

## 5. 用户 XnView 样本

样本：`XnViewMP_v1.11.6.0_setup_x64_mefcl.exe`，60,683,048 字节；它是用户提供的封装包，不将它作为 XnView 官方发行来源。

```text
输入 SHA-256
c497589599950fbe7ee034d51d0890b5c8c3b474f927267bddca89ed6562856a
```

外层 NSIS first header 位于 **204288**，元数据 9312 字节，标记版本 **3.12**；包内 CRC32 验证通过。它前面的附加区虽然含 7z 标记，也不能因此忽略 NSIS 文件指令。

已提取并逐字节对照原包的 3 个不同文件，总计 **60,469,400** 字节：

| 输出 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `plugins/System.dll` | 12288 | `996645d7f16a6294babb6da83478062aed7b72318d7a51b66e90ab5d3edc51a5` |
| `plugins/XnViewMP-win-x64.exe` | 60453016 | `0116231a5e626bbc5efb9da0c180d43dd452dcafa35c869886fea4fd93ac6031` |
| `plugins/Process.dll` | 4096 | `217c763c4f2a929df1493906d5dd3f9ad99a999efc407de9b6f4a6025fae3037` |

原文件在提取前后 SHA-256 一致。`tests/verify-nsis.py` 独立读取原包的未压缩数据块作为基准，再与产品实际输出比较；无安装器、内嵌 EXE 或 DLL 被执行。

大文件是内嵌 Inno Setup 包，数据版本 **6.5.0**。0.3.1 已适配该结构并自动展开，输出 **1080 个应用文件 / 190,857,396 字节**，逐个通过原包 SHA-256 核对。主程序为 `XnViewMP-win-x64_extracted/app/xnviewmp.exe`，14,398,288 字节。两层累计 **1083 个文件 / 251,326,796 字节**；原始安装器保留在 `plugins` 中，不应把它当作应用程序启动。

`tests/verify-nsis.py` 还独立读取内层的位置记录，并核对所有输出的大小/摘要集合与生产方记录一致。包中同目标的条件/重复条目仍按统一规则保存在 `_variants`，没有执行安装脚本来挑选或覆盖。

## 6. 下一阶段

下一步实现 ZIP/7z 内嵌归档，并针对 Electron-builder 近期离线包核验最终应用文件。在线下载、加密、插件生成内容及重排字节码等变体需要各自的明确边界，不通过运行安装器来补齐提取结果。0.3.1 的结构差异、递归规则与产物信息见 [实现说明](10-nested-extraction.md)。

便携目录为 `out/dist/win-x64-release/`。本轮未做新的 Explorer 拖拽及通知横幅视觉验收；通知复用既有实现。未提交或推送 Git。
