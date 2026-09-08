# Inno 外置分卷与 CAB 跨卷续接

功能纳入 v0.8.0，见 [版本说明](releases/v0.8.0.md)。下文的本地开发状态、版本号和产物摘要保留为发布前历史验证记录。

本地开发，版本保持 0.7.0，尚未发布。安装器识别、卷链、文件规划与校验由本项目完成；底层解码继续使用静态压缩库及系统 FDI，不调用外部解包工具，不运行安装器。

## 使用与支持范围

| 输入 | 行为 |
| --- | --- |
| Inno `setup.exe` + `setup-1.bin`、`setup-2.bin` 等 | 拖入 EXE，按头部与位置表定位外置卷并提取 |
| Inno `setup-1a.bin`、`setup-1b.bin` 等 | 按 SlicesPerDisk 计算卷名，支持每磁盘 1–26 个分卷 |
| 标准 Microsoft CAB 1.3 卷链 | 可拖入首卷、中间卷或末卷；回到起始卷后顺序提取，恢复跨卷文件 |
| MSI 内嵌、外置及混合 CAB 卷链 | 仅通过 Media 表解析卷名，将跨卷 CAB 文件关联到 File 表目标目录 |

所有外置卷必须放在主包同目录，保留原文件名；不搜索其他目录、不联网寻找缺卷。单次只拖一个主包或一个 CAB 卷；批量拖入同一 CAB 集合的多卷会分别执行任务。缺卷时提示具体名称；卷内容、集合编号或续接文件清单不符时拒绝当前层，保留已有输入与其他任务结果。

Inno 支持已适配标准数据布局上的外置载荷，包括无压缩、Deflate、bzip2、LZMA、LZMA2，以及普通/固实块。外置分卷和 `[Files]` 的 `external`/下载条目不同：后两者仍未实现。修改版、加密载荷及未适配的头部仍不支持。

CAB 覆盖无压缩、MSZIP、LZX。这里只增加独立 CAB 与 MSI 的卷来源适配；Burn 内部 CAB 容器跨卷、InstallShield 私有 CAB、分卷 ZIP/7z 仍未支持。跨卷 SFX 没有专项验证，不作为本轮支持承诺。

CAB 卷引用按系统 ANSI 代码页解释；物理卷名无法用该代码页表示时明确报错。父目录仍支持 Unicode，普通单卷 CAB 不依赖物理文件名的 ANSI 转换。

## 解析与资源处理

Inno 根据数据布局分别读取旧式 `idska32` / 32 位长度和新版 `idskb32` / 64 位长度卷头。文件位置的 FirstSlice、LastSlice、StartOffset 与压缩长度都必须落在实际卷范围内。将卷头排除后建立逻辑偏移，先校验块标识与重叠，再用 64 KiB 缓冲拼接当前压缩块到受控临时文件。解码后仍校验文件 SHA-1 / SHA-256，完成后释放该块缓存。分卷输入持有只读句柄及父目录锁。

CAB 先核对集合 ID、卷编号、链接与跨卷文件的名称/大小/偏移，再向 FDI 提供已验证的内存视图。前卷链接可能回指跨多卷文件的起始卷，所以不能简单要求它总指向相邻编号。起始卷确定后沿 next 填齐中间卷；未连接、循环、缺失或错集合明确失败。FDI 的换卷回调只接受已预检的名称，不向它开放任意文件系统读取。

MSI 将卷名匹配到 Media 表，外置卷仅从 MSI 同目录读取，内嵌流写入受控临时缓存。内外源同名造成歧义或 Media 未声明引用时拒绝；不根据缺失的流名尝试包外文件。恢复后的文件仍按 MSI File/Directory 表映射并验证已有 MsiFileHash。

没有新增包体积或输出文件数上限。仍保留整数、路径、元数据与循环保护：Inno 分卷描述和 CAB 集合文件表/名称各有 64 MiB 元数据预算；单 CAB 受系统 FDI 有符号 32 位定位约束。Inno 单个压缩块与 CAB 输入仍需连续映射地址空间；x86 大块可能失败，宜用 x64。MSI 卷链可能同时占用多份压缩 CAB 的临时磁盘空间。详见 [大包资源边界](17-large-packages.md)。

准备阶段展示实际读取字节，不估计总耗时百分比；写入阶段沿用实际输出字节进度。缺卷发生在目录预检或媒体预检阶段，损坏数据可能在解码或哈希校验阶段发现，日志记录相应步骤。

## 验证方式

`tests/volume_integration.py` 用官方 ISCC、MakeCab 及 `tests/make_spanned_msi.ps1` 生成惰性包：源文件包含跨多卷的大文件、首尾小文件和空文件。逐项比较原始字节、输出 SHA-256、Inno 源摘要或 MSI 文件哈希；同时核对输入未改变、失败未提交输出、临时缓存无泄漏。仅执行生成器与 Extract。

完整专项共 38 例：Inno 15 例，CAB/MSI 23 例。Inno 覆盖五种压缩 × 普通/固实组合，以及缺卷、截断、错标识、交换卷和数据损坏。CAB 覆盖三种压缩 × 首/中/末卷入口，大小写卷名、路径穿越、循环、续接名称不一致、缺卷、错集合/编号和损坏；MSI 覆盖内嵌、外置、混合、缺外置卷及未声明媒体。

本地专项实测：x64、x86 分别用 Inno 6.7.3 完成全部 38 例；另以 x64 验证 Inno 6.0.5 的 15 例、x86 验证 Inno 6.6.0 的 15 例。共 106 次均得到预期结果，包含预期拒绝的反例；不把它们算作 106 个成功解包的真实软件。

最终 x64 / x86 全量 CTest 各 14 项通过，无失败、无跳过，用时约 255 / 251 秒。两份便携 EXE 与对应测试二进制 SHA-256 一致，静态 CRT、仅系统 DLL、ASLR/DEP/CFG 均通过核对。版本未增加，没有创建或推送标签、没有发布 Release。

```powershell
python tests/volume_integration.py --executable out/build/win-x64-release/bin/Extract.exe --work-root out/build/win-x64-release/volume-fixtures
# 指定其他官方编译器，单独验证 Inno 分卷：
python tests/volume_integration.py --executable out/build/win-x64-release/bin/Extract.exe --work-root out/build/win-x64-release/volume-fixtures-extra --compiler 'C:\Tools\Inno\ISCC.exe' --only inno
```

普通 CTest 增加 `volume_integration`，工具齐备时共 14 项；发布工作流要求全部通过且无跳过。专项缺少 ISCC 返回 77，不计为通过。具体架构、编译器、完整用例、产物摘要与运行结果见 [验证数据](data/validation-split-volumes.json)。生成样本通过不等于所有第三方重打包均可提取；Ant 修改版尚未解决，且本轮未取得原文件复测。ARM64 实机及干净 Windows 10/11 桌面验收仍待补充。

## 结构依据

- Inno 官方固定源码：[6.0.5 Struct.pas](https://github.com/jrsoftware/issrc/blob/is-6_0_5/Projects/Struct.pas)、[6.7.3 Shared.Struct.pas](https://github.com/jrsoftware/issrc/blob/is-6_7_3/Projects/Src/Shared.Struct.pas)、[6.7.3 Setup.FileExtractor.pas](https://github.com/jrsoftware/issrc/blob/is-6_7_3/Projects/Src/Setup.FileExtractor.pas)。
- Microsoft [CAB 格式规范](https://download.microsoft.com/download/4/D/A/4DA14F27-B4EF-4170-A6E6-5B1EF85B1BAA/%5BMS-CAB%5D.pdf)、[FDI 通知](https://learn.microsoft.com/en-us/windows/win32/api/fdi/ns-fdi-fdinotification)、[MSI Cabinet Files](https://learn.microsoft.com/en-us/windows/win32/msi/cabinet-files)，结合 SDK `fdi.h` 与 MakeCab 实际产物核对。
