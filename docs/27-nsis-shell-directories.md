# NSIS 系统目录与外壳行为

Extract 静态提取包内文件，不执行安装脚本，也不把文件部署到本机系统目录。“文件提取完成”表示包内载荷及已识别的内层文件提取完成，不表示已生成与原启动器行为一致的便携程序。

## 目录还原

本节说明解析器还原的逻辑位置。`--layout original` 直接按这些位置保存；默认精简布局可能将明确识别的应用提升到根目录，附加文件归入 `_extra`。实际位置见报告的 `files[].path`，原始位置和表达式仍保留。详见[目录布局](28-compact-layout.md)。

Shell 编码包含当前用户和所有用户两个目录标识。解析器根据 `SetShellVarContext current/all` 在该指令位置的静态状态选择逻辑目录：

| 常量 | current | all |
| --- | --- | --- |
| `$DOCUMENTS` | `User/Documents` | `Public/Documents` |
| `$APPDATA` | `AppData/Roaming` | `ProgramData` |
| `$LOCALAPPDATA` | `AppData/Local` | `ProgramData` |
| `$DESKTOP` | `User/Desktop` | `Public/Desktop` |
| `$STARTMENU` / `$SMPROGRAMS` / `$SMSTARTUP` | `AppData/Roaming/Microsoft/Windows/Start Menu` 及对应子目录 | `ProgramData/Microsoft/Windows/Start Menu` 及对应子目录 |

同时映射模板、收藏夹、音乐、图片、视频、管理工具、Windows、System、用户配置、字体、SendTo、Recent、Quick Launch，以及已识别的 `USER*` / `COMMON*` 固定目录。名称描述逻辑位置，不承诺操作系统重定向、访问失败回退或文件系统视图的实际路径。注册表编码的 ProgramFiles/CommonFiles 及其它未识别编码仍保留原始 `SHELL_<编码>` 表达式，文件进入 `_unresolved`，不会伪装成已解析。

- `SetOutPath` / 完整 `StrCpy` 确定的路径保留赋值时的用户上下文，之后切换上下文不会重定向已有 `$OUTDIR`。
- 分支合流必须一致；不同用户上下文、间接跳转等不确定情况不猜测，相关文件保留到 `_unresolved`，结果为部分完成。
- 直接调用向被调用函数传递入口上下文。多个调用入口合流不一致时保留歧义；如果函数可能修改上下文，返回后保守视为未知，后续显式赋值可重新确定。
- 不推断区段选择或回调执行顺序。包内存在上下文修改或插件调用时，区段和回调入口保守从未知状态开始；否则使用 NSIS 的默认 current。不会把 `.onInit` 的赋值盲目套用到所有区段。
- `sourceExpression` 保留例如 `$DOCUMENTS[all]`、`$APPDATA[current]` 或 `$DOCUMENTS[unknown]`；未知编码保留数字供诊断。
- 包含系统目录且载荷分布在多个逻辑目标位置时，成功入口指向结果根目录，避免只打开 `app` 而隐藏其它文件。原有纯内嵌安装器的结果定位保持不变。

## 外壳行为报告

`_extract-report.json` 和 `--list` 输出增加：

- `runtimeNotice`：提示真实系统位置未写入、外壳操作未执行以及直接运行效果可能不同；提取时同步写入控制台诊断和任务摘要。
- `runtimeActions`：静态观察到的 `launch`、`launch-shell`、`delete-file`、`remove-directory` 指令。`instruction` 为零起始指令编号，`target` 为符号展开后的目标或未解析表达式，`targetResolved` 只表示目标字符串的静态解析情况，`waitsForExit` 表示等待参数，`recursive` 表示递归删除目录参数。

这些记录按指令表排序，不是执行轨迹，不断言操作一定发生，也不推断哪些删除操作一定在启动之后。启动记录不执行或改写命令；Shell 启动仅记录目标、等待标志，不复原动词、参数等完整调用语义。未知变量仍标记为未解析。删除、注册表修改、配置生成和授权部署均不会执行，不根据文件名判断许可证有效性。

## 保留编译脚本

每个受支持且可提取的 NSIS 层都会保存 `_extract-script/`。原始布局中位于各层结果根目录；精简布局中根包脚本仍在结果根目录，内层包脚本位于 `_extract-packages/pN/_extract-script/`，由该层报告的 `compiledScript` 指向：

- `nsis-script.txt`：UTF-8 静态指令记录，包含所有指令的零起始编号、名称、数值操作码、全部六个有符号参数，以及可识别的字符串参数预览。另存块偏移、区段和回调入口、完整原始字符串表与语言字符串索引。
- `nsis-header.bin`：原样保存已经解压的完整 NSIS header，包含指令、字符串、区段、页面、语言表等编译元数据。不是启动器 EXE，不包含文件数据块；压缩前的原始数据仍在输入包中。

编译时丢失的源码注释、宏、函数名和标签无法从这些数据精确恢复，因此不生成可执行 `.nsi`，也不声称得到原始源码。字符串预览最多读取 256 个编码单元，保留变量和语言引用，不执行替换；`[context]` 表示依赖用户上下文。完整原始字符串表按 UTF-16 编码单元或 ANSI 字节转义保存，不截断内容，未解释指令仍保留原始操作码和参数。未使用字符串中的异常编码不会导致已支持载荷提取失败。

`compiledScript` 在 `--list` 和报告中描述文件路径、指令数量、元数据大小及 SHA-256；文本大小与 SHA-256 在成功写出后填写。辅助文件不计入 `files`、载荷数量和 `totalBytes`。提取前按实际文本和元数据大小额外检查磁盘空间，按小块写出，支持取消及失败回滚。包内文件若与 `_extract-script` 根目录冲突，按现有规则保留到 `_variants`，不会覆盖或丢弃。

## ReNamer 样本

用户提供的 `ReNamerPro_7.10.0.0.exe`，SHA-256：

```text
a49b423a0060de8c2418f36742ccd6e2ce7a1819dc58b8a0fe7bda4c82e7d1bf
```

包内 49 个文件、7,344,478 字节，包含公共文档目录下的 `den4b/ReNamer/License.key` 和当前用户 Roaming 目录下的 `ReNamerPro_mefcl`。静态脚本存在启动并等待主程序、调用清理函数的操作。原始布局保留两类目标目录；默认精简布局把主程序目录内容提升到根目录，将授权文件保存为 `_extra/PublicDocuments/License.key`，报告仍保留原路径和行为记录。授权文件不部署到本机，也不承诺提取后的程序显示 Pro。

## 验证与依据

`tests/nsis_integration.py` 使用官方 NSIS 编译器生成惰性样本，仅运行编译器和 Extract。覆盖上下文切换、已赋值 OUTDIR、默认上下文、变量复制、分支合流、函数调用及多调用合流、回调歧义、固定用户/公共目录、外壳启动等待/清理记录、动态目标、未知 Shell 编码，以及派生的旧式 ANSI 字符串编码。`tests/nested_integration.py` 额外确认系统目录中的卸载辅助文件不会遮蔽实际应用的打开入口。

脚本保存测试逐条比较文本指令与二进制指令表，并从文本的原始字符串表重建字节后核对原始表；覆盖 stored、三种压缩方式及 Solid、Unicode/ANSI、辅助目录冲突、未使用的异常字符串及未知操作码。另用官方编译器生成 FileOpen、FindFirst、文件时间/版本和注册表操作，核对字符串参数位置，所有样本始终不执行。ReNamer 的 131 条指令和 9,734 字节解压元数据完整保留，原始 49 个载荷不变；见[脚本保存验证数据](data/validation-nsis-script.json)。

真实样本已独立解码原包全部 49 个载荷，与新输出逐字节比较，并核对路径、大小和 SHA-256；原包与真实系统目标位置在提取前后未变化。记录见[样本验证数据](data/validation-nsis-shell-directories.json)。未执行输入或提取后的程序，未验证 Pro 运行状态。

格式依据：[NSIS v3.12 Shell 编码解析](https://github.com/kichik/nsis/blob/v312/Source/exehead/util.c)、[目录常量表](https://github.com/kichik/nsis/blob/v312/Source/build.cpp)、[用户上下文](https://github.com/kichik/nsis/blob/v312/Source/exehead/api.h)、[执行和等待参数](https://github.com/kichik/nsis/blob/v312/Source/exehead/exec.c)。
