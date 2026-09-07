# 常见封装与目录样本验收

日期：2026-09-05。版本：0.4.0。

## 本轮范围

| 格式或行为 | 已实现内容 |
| --- | --- |
| ZIP / ZIP64 | 自研中央目录、本地记录、数据描述符、ZIP64 扩展及偏移校验；stored、Deflate、bzip2 |
| ZIP 路径 | UTF-8、CP437、Info-ZIP Unicode 路径扩展；危险路径/链接拒绝，同名变体保留 |
| 7z | LZMA SDK 原生 C 归档基础层，Copy/LZMA/LZMA2、普通/固实块、BCJ/BCJ2 等 SDK 支持的过滤器 |
| PE SFX | 有界扫描 PE 节末后的附加区，读取 ZIP/7z；不执行 SFX 配置或 RunProgram |
| Electron / NSIS | BrandingText 可自定义；`plugins/app-64.7z` 自动展开到独立目录 |
| NSIS / Tauri | 显式 OUTDIR 赋值跨分支合流，Pebble 主程序路径恢复为 `app/pebble.exe` |
| 卸载器 | 内嵌 NSIS 卸载器原样保留，`nestedPackages.status=preserved`，不作为应用安装器递归 |

ZIP 结构依据 [PKWARE APPNOTE 6.3.10](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)，ZIP 封装解析由本项目实现；7z 使用已固定来源的 [LZMA SDK 26.03](https://www.7-zip.org/sdk.html) C 归档底层。NSIS 安装器解析、SFX 定位、递归决策、输出路径与校验流程由本项目实现，没有调用外部解包程序。开发测试使用的官方编译器和 7zr 不进入发行目录。

## 目录与完成状态

NSIS 支持用 [BrandingText](https://nsis.sourceforge.io/Docs/Chapter4.html#brandingtext) 修改默认品牌文字。版本文字缺失时不猜具体补丁版本，保留兼容布局标签，并继续验证块边界、Unicode 字符串、语言表和指令编号。

目录分析只关注字节码显式写入 OUTDIR 的行为。合流时各分支必须给出相同值；内部辅助函数可证明没有显式目录写入时保留目录，否则回退。动态地址和无法证明的路径保留在 `_unresolved`。完整变量复制仍按基本块分析，不执行安装条件、插件代码或任意副作用；不能据此保证实际安装时的全部动态行为。

NSIS 非 `app` 根下的 ZIP/7z 会递归展开；普通应用目录中的归档资源（例如应用自己的资源包）保持原样。EXE/MSI 仍按结构检查安装器或 SFX。原始内层包保留，每层报告记录父子关系；已识别内层损坏、不支持或超限继续传播 partial。

## 校验与资源限制

ZIP 文件 CRC 与本地头/中央目录/描述符相互核对；7z 由 SDK 检查头和存在的固实块 CRC，再由本项目检查存在的文件 CRC。`sourceHashAlgorithm=CRC-32` 表示包内校验和，不是来源认证；未提供文件 CRC 的条目不会声称验证了源摘要。所有写出文件仍计算 SHA-256。

取消输入 512 MiB、累计 8 GiB 和 10000 文件固定阈值；自动递归仍限制为 32 个包、4 层。ZIP 中央目录和 7z 常驻元数据分配限制 64 MiB；7z 辅助解码分配限制 256 MiB。固实块写入受目录锁保护的临时磁盘映射，展开大小按 64 位文件范围、当前架构可表示范围及实际磁盘空间检查，离开当前块后清理；不分配与整块输出等大的堆缓冲。缓存与最终输出可能同时占用磁盘。

仍不支持 RAR、加密/多卷归档、PPMd/Deflate64/Zstandard 等未启用方法、WiX Burn 和任意在线下载封装；不模拟插件生成文件、动态安装行为或覆盖顺序。空目录不单独恢复，链接和特殊文件拒绝。未引入 worker 或硬超时。

## 自动测试

Debug 与 Release 均通过 6 项 CTest：路径、MSI、Inno、NSIS、嵌套安装包、归档。无跳过。

- NSIS 每轮 92 次 Extract 调用，增加自定义 BrandingText、相同目录合流、保留目录的辅助函数、改变目录的函数和动态 OUTDIR 写入。
- 嵌套安装包保留 14 种场景，损坏归档现在被识别为损坏而非未实现格式。
- 归档每轮 50 种场景：ZIP 三种方法、描述符、ZIP64 本地/尾记录、UTF-8、SFX、空文件/目录、冲突；7z 三种方法 × 普通/固实、BCJ/BCJ2、SFX、NSIS 嵌入 ZIP/7z；CRC、截断、分卷、加密/未知方法、越界、重解析点、符号链接及穿越路径反例。
- 测试既对照打包前原始内容，也独立构造 stored 7z 元数据检查超限与危险条目。正常/异常结束后均检查缓存和失败暂存清理。

## 用户安装包验证方法

扫描 `out/dist/win-x64-release/` 顶层的安装 EXE，排除 Extract 自身。只运行 Extract 和已核对来源的测试工具，不运行输入安装器或提取出的 EXE/DLL。

`tests/verify-installers.py` 逐包建立新结果，检查所有文件大小及 SHA-256；Inno 再从原输入独立读取位置表的 SHA-1/SHA-256；7z 再用官方 7zr 进行完整性测试并列出 CRC，对照实际输出。每个原输入前后摘要一致。结果保存到 `out/installers-0.4.0-release.json`，输出保存在各安装包旁，同名自动加序号。

本轮实际结果（合计含保留的外层安装器和插件）：

| 输入 | 外层 / 内层文件数 | 合计字节 | 结果 |
| --- | ---: | ---: | --- |
| ECHO-NEXT-Setup-26.8.2.exe | 11 / 430 | 1,159,296,535 | 应用 7z 完成；1 个卸载器路径 unresolved，整体 299 |
| Pebble_0.1.4_x64-setup.exe | 7 / — | 16,212,862 | 完成，0 |
| Textify.exe | 6 / — | 236,025 | 完成，0 |
| vic-diary-2.0.0-setup.exe | 10 / 90 | 328,953,160 | 应用 7z 完成；1 个卸载器路径 unresolved，整体 299 |
| XnViewMP_v1.11.6.0_setup_x64_mefcl.exe | 3 / 1080 | 251,326,796 | 完成，0 |

ECHO NEXT、vic-diary 的实际应用位于各自结果的 `app-64_extracted/`，不是 `plugins/app-64.7z`。Pebble、Textify 位于 `app/`。本轮 XnView 因已有同名结果，创建了 `XnViewMP_v1.11.6.0_setup_x64_mefcl_extracted (2)/XnViewMP-win-x64_extracted/app/`。

两个 Electron 包的卸载器文件内容已经提取并计算摘要，但目标路径无法证明，因此保留在 `_unresolved`；它们的 7z 子报告均为 complete。这个限制不会被改写为整体成功，日志与通知仍使用部分完成状态。

输入 SHA-256：

```text
ECHO NEXT  131ca4727757e6c9a366013feb0dd782946a9e2ab200afc27e73a47444b22698
Pebble     5f4054c93cd3933a930daedf8b2dd3848b677617b2f5df0148de349cf670d44e
Textify    cef497c5237f4b10b09fd4d45b051f3777f9705debb2bb6bf7dcf7ffe400b01e
vic-diary  4f375a152ad4ee4ece439c8927f4150e1517ff9713ad45de2e4cd9bec9de8221
XnView     c497589599950fbe7ee034d51d0890b5c8c3b474f927267bddca89ed6562856a
```

```powershell
python tests/verify-installers.py --executable out/dist/win-x64-release/Extract.exe `
    --input-directory out/dist/win-x64-release --output-parent out/dist/win-x64-release `
    --sevenzip out/references/lzma2603/bin/x64/7zr.exe `
    --report out/installers-0.4.0-release.json
```

## Release 产物

`out/dist/win-x64-release/Extract.exe`，0.4.0，635392 字节；包含用户提供的 5 种尺寸程序图标。SHA-256：

```text
7d1a8e9579ca5cfb0f285be3d7c5a53e45e3a820b8a6960a582c0ee20e5224f1
```

本轮验证了静态提取与内容校验，未启动应用验证运行，也未重新进行 Explorer 拖拽或通知横幅的视觉验收。未提交或推送 Git。
