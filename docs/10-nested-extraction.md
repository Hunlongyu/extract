# 内嵌安装包自动展开与 Inno 6.5.0

日期：2026-09-05。版本：0.3.1。

本文件保留此版本的范围和产物摘要。当前 0.4.0 已增加归档层，详见 [常见封装与样本验收](11-common-wrappers.md)。

## 问题与结果

用户的 `XnViewMP_v1.11.6.0_setup_x64_mefcl.exe` 是 NSIS 外壳，外层的 `plugins/XnViewMP-win-x64.exe` 仍是 Inno 安装包。只提取这三个外层文件无法满足获取应用内容的目标。现在拖入原包后，自动完成 NSIS → Inno 6.5.0 → 应用文件，并将单个成功任务的打开目标指向内层 `app` 目录。

| 内容 | 文件数 | 字节数 |
| --- | ---: | ---: |
| NSIS 外层，包括保留的内层安装器 | 3 | 60,469,400 |
| Inno 应用内容，包括保留的重复/条件条目 | 1080 | 190,857,396 |
| 累计 | 1083 | 251,326,796 |

应用主程序是 `XnViewMP-win-x64_extracted/app/xnviewmp.exe`，大小 14,398,288 字节，SHA-256：

```text
22cb8b9c710257e52ab5d4d7a5b43c5758bd187302375a7376250068703b740e
```

这次验收检查了静态文件内容，没有启动用户安装器、应用程序或插件。条件和同目标条目继续保留在报告及 `_variants`，不模拟安装覆盖规则，不承诺所有软件可以直接便携运行。

## Inno 6.5.0 结构

参考生产方 [is-6_5_0](https://github.com/jrsoftware/issrc/tree/is-6_5_0)，固定提交 `9e493a15d90fc9c538f79fe7857804ec7571eb05`。对照 `Projects/Src/Shared.Struct.pas`、`Shared.SetupEntFunc.pas` 与 `Compression.Base.pas`，保持独立解析实现。

- 精确匹配 `Inno Setup Setup Data (6.5.0)`，不是套用 6.5.2 的布局。
- Setup 固定字段区为 38 字节，比 6.5.2 少两个背景色字段，共 8 字节。
- FileLocation 为 85 字节，`StartOffset` 为 32 位；6.5.2 对应 89 字节和 64 位偏移。
- 仍为 loader v2、SHA-256 文件摘要及既有块压缩/过滤方式。

测试编译器来自 [官方 6.5.0 发行包](https://github.com/jrsoftware/issrc/releases/tag/is-6_5_0)，下载文件 `innosetup-6.5.0.exe` 的 Authenticode 状态 Valid，签名人为 Pyrsys B.V.；SHA-256：

```text
79b7a1063b3888bb8eceb44a8c28e90ccdaa22ac71f9272de1dac79b42941dbd
```

使用 Extract 静态提取编译器，测试仅运行其中的官方 ISCC，不运行安装器。测试工具不随产品发布。

## 递归规则

`core/tree.cpp` 统一协调 MSI、Inno、NSIS：每层安全提取并提交，释放映射和 Solid 缓存，再从本层文件清单筛选 EXE/MSI，验证安装器签名。普通应用 EXE 不作为安装包处理。NSIS 非 `app` 目录的 ZIP/7z 签名仅用于提示未展开，目前没有归档解包实现。

每个根输入共享 8 GiB 输出、10000 个文件、16 个包和 4 层的上限，均包含根层和保留的内层安装包；新层落盘前检查累计预算。按 SHA-256 复用相同内容的结果，检查当前活动链。打开内层输入后复核上层记录的 SHA-256，层间被修改时拒绝继续。

每层沿用同名自动编号、路径校验、目录锁和失败暂存清理。内层失败保留已提交的外层与原始内层包，不留失败子层的半成品。报告用临时文件和同目录原子重命名更新：

| 字段 | 含义 |
| --- | --- |
| `files` / `totalBytes` | 本层文件与字节数 |
| `nestedScanned` | 是否完成内层候选检查；`--list` 为 false |
| `nestedComplete` | 已识别内层是否全部完成 |
| `nestedPackages` | 相对输入、相对输出、格式、状态与失败原因 |
| `treeFileCount` / `treeTotalBytes` | 本层及新展开子树的实际输出；复用结果不重复计数 |
| `status` | 动态路径、已识别内层失败/不支持/超限均为 partial |

内层异常向祖先传播，进程返回 299；通知报告部分完成，点击打开任务记录查看具体原因。普通成功任务指向其 `app`（如存在）；只有一个成功内层的 NSIS 外壳直接指向内层应用目录。`--list` 只列当前层，不创建结果或递归扫描。

## 验证

- Debug、Release 的 CTest 均通过路径、MSI、Inno、NSIS、嵌套提取五项，无跳过。
- 官方 Inno 6.5.0 另外验证五种压缩 × 普通/solid 共 10 包，每轮 33 个异常样本、62 次 Extract 调用。
- `tests/nested_integration.py` 的 14 种场景验证 NSIS → Inno、NSIS → NSIS → Inno、普通 EXE、重复内容、内层未知版本、摘要错误、动态目录、ZIP/7z 提示，以及深度/文件数/字节数/包数限制。测试仅执行官方编译器和 Extract。
- `tests/verify-nsis.py` 独立读取 XnView 外层未压缩文件块逐字节比较，读取内层 85 字节位置记录作为摘要基准，核对全部 1080 个输出文件及主程序；输入前后摘要一致。

复现真实样本（输出父目录由脚本创建）：

```powershell
python tests/verify-nsis.py --executable out/build/win-x64-release/bin/Extract.exe `
    --input 'D:/Downloads/XnViewMP_v1.11.6.0_setup_x64_mefcl.exe' `
    --output-parent out/xnview-verification
```

Release 0.3.1 可执行文件已嵌入 `resources/logo.ico`，为 561664 字节，只导入 Windows 自带 DLL，SHA-256：

```text
50018a747baef8d77db463b419ed579c829828e81df320af86bbdeb02a8d7257
```

便携目录为 `out/dist/win-x64-release/`。本轮通过命令行调用与拖拽共用的提取入口，未重新进行 Explorer 拖拽、通知横幅的视觉验收。ZIP/7z 展开、Electron 完整支持、worker 和硬超时仍待实现。
