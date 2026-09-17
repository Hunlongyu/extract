# 精简输出目录

默认拖拽及命令行提取使用 `compact` 布局。目标是减少包装层和逻辑系统目录造成的多次进入，同时保留全部载荷、程序内部相对目录及每层脚本记录。

## 使用方式

```powershell
# 默认精简布局
Start-Process .\Extract.exe -ArgumentList '--quiet "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait

# 保留解析器原有逻辑目录及嵌套层级
Start-Process .\Extract.exe -ArgumentList '--quiet --layout original "D:\Downloads\setup.exe"' -WindowStyle Hidden -Wait
```

也可显式传入 `--layout compact`。参数只作用于本次任务；重复指定、缺值、未知值返回 160。`--list` 仅显示原始逻辑路径，因为完整精简规划需要知道内层包的提取结果。它的 `requestedLayout` 记录选项，最终保存位置以提取报告为准。

## 规划规则

- 整棵提取树完整时，解析器确定的 `app`、`app-x86`、`app-x64`、`app-arm64` 可作为应用目录。NSIS 静态启动记录能精确对应到包内 EXE 时，也可确定主程序目录；不执行命令，不猜测动态变量。
- 只有一个明确应用时，提升它的目录内容，保留其下的 DLL、资源、语言、配置等相对结构。识别的架构目录分别使用 `x86`、`x64`、`arm64`。
- 多个包都有明确应用时，分别保存到 `_packages/pN-包名/`，不混合文件。NSIS/CAB 包装中的完整 ZIP/7z 可作为整体应用载荷提升，但保留该归档内部目录。
- 应用目录之外的文件按来源归入 `_extra`，每组裁去共同的上层目录，例如 `_extra/PublicDocuments/License.key`；内层包附加文件另加 `pN`，避免互相覆盖。原始内嵌安装包也保留。
- 未知路径、部分完成、多个不同启动位置、资源与保留目录重名、文件/目录冲突时，保守保留相对结构。内层包并列放入 `_packages/pN-包名/`；仍有冲突时走现有 `_variants` 规则，绝不覆盖载荷。
- 独立 ZIP/7z（含其 SFX）与 MSIX/APPX 保持原有包内层级，不执行精简整理。默认成功入口打开精简结果根目录。

ReNamer 样本的精简结果：

```text
ReNamerPro_7.10.0.0_extracted/
  ReNamer.exe
  Settings.ini
  Languages/
  Scripts/
  Translits/
  _extra/PublicDocuments/License.key
  _extract-script/nsis-script.txt
  _extract-script/nsis-header.bin
  _extract-report.json
```

这只改变输出组织方式。Extract 不执行输入、脚本或载荷，不写真实系统目录，不恢复原包的授权部署或运行时行为。

## 报告与脚本

精简结果的根 `_extract-report.json` 是完整载荷清单：

| 字段 | 含义 |
| --- | --- |
| `layout` / `requestedLayout` | `compact`；保守回退原因另记在 `notes` |
| `pathBase` | `extraction-root`，路径相对整个结果根目录 |
| `files[].path` | 实际保存位置 |
| `files[].package` | 来源层 `p0`、`p1` 等，根包为 `p0` |
| `files[].id` | 来源层与原条目标识的组合 |
| `originalPath` / `sourceExpression` | 原逻辑路径及原表达式，不因提升目录而改写 |
| `totalBytes` / `treeTotalBytes` / `treeFileCount` | 全部已提取层载荷的累计信息；辅助报告和脚本不计入 |
| `nestedPackages[].input` / `output` / `report` | 内嵌包、提取文件共同父目录及对应层报告在根目录中的位置；共同父目录可为 `.` |

根包 NSIS 脚本位于 `_extract-script`。每个内层包的报告位于 `_extract-packages/pN/report.json`，NSIS 脚本在其旁边的 `_extract-script`；其 `files[].path` 和 `compiledScript` 路径同样以整个结果根目录为基准。复用同一内层包时指向同一报告。原始布局保持各层报告及原文件标识，`pathBase` 为 `report-directory`。

脚本指令、元数据及载荷内容不改写，复制前后校验大小和 SHA-256。NSIS 保存范围详见[系统目录与外壳行为](27-nsis-shell-directories.md)。

## 暂存、空间和失败

每层先在受保护的临时目录完成解码和校验，收集全部结果后统一规划；随后分块复制到最终临时目录、复核摘要并写出映射报告，最后提交结果。目录整理会增加读写和哈希耗时；整理期间大约需要两份展开内容的磁盘空间，另加解码器缓存和报告。逐层及最终整理前都检查空间，但不预留磁盘配额。

内层解析失败时，可将已解出的文件按保守布局提交为部分完成。最终整理阶段取消、磁盘不足或校验失败时，未提交结果回滚。清理仅处理本任务登记且文件身份、创建时间仍一致的项目，不递归删除未知文件；被替换或无法验证的项目保留。进程异常时由现有工作进程恢复逻辑清理登记的临时文件。

磁盘紧张或需要逐层原貌时使用 `--layout original`，可避免整理副本，保留原来的逐层提交方式。没有新增统一输入/输出体积限制，资源约束仍见[大包处理](17-large-packages.md)。

## 验证

`tests/layout_integration.py` 使用官方 NSIS 编译器生成惰性样本，覆盖两种布局、原始列表、系统目录、歧义与冲突、三层包装、多应用、架构、复用内层包、归档层级、重名输出、非法参数，以及整理阶段取消和强制终止。逐文件核对大小与哈希，核对内层报告和脚本路径。`path_policy` 另验证暂存清理保留替换文件与未登记文件。

真实 ReNamer 样本采用独立 Deflate 解码核对全部原始载荷、131 条指令和编译元数据，并核对提取前后真实系统目录未变化。证据见[精简布局验证记录](data/validation-compact-layout.json)。全部验证均不运行输入安装器、脚本或提取后的程序。
