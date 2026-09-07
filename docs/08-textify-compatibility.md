# Textify 兼容修复

日期：2026-09-05。版本：0.2.1。

用户拖入 Textify 后，任务 `C01757D7-7060-4AA6-AC27-B786EAF20ECE` 报告“当前不支持此 Inno loader 布局”。原因是 0.2.0 只实现了 loader v2，样本实际使用 loader v1 与 `Inno Setup Setup Data (6.1.0) (u)`。

原始偏移表 CRC 正确。这是兼容分支缺失，不是依据已发现的数据损坏作出的拒绝。`6.1.0 (u)` 是数据标识，不能据此断定安装包的编译器产品版本；生产方 6.2.2（2023-02-15）仍使用这套结构，处于项目的近期样本窗口。[官方结构定义](https://github.com/jrsoftware/issrc/blob/is-6_2_2/Projects/Struct.pas)、[官方版本记录](https://jrsoftware.org/files/is6.2-whatsnew.htm)

## 修复内容

- 解析并校验 loader v1 的 44 字节偏移表、32 位文件偏移及其 CRC；保留 v2 分支，未知 loader 显示具体版本号。
- 增加精确 Unicode 数据标识 `6.1.0 (u)`；不顺带放行 ANSI、Inno 5 或未知布局。
- 适配 30 个头部 Unicode 字符串、16 个计数、旧向导/加密标志、10 个文件字符串，以及 74 字节位置记录。
- 旧位置表中 CALL/JMP、加密和压缩的位位置不同，转换到共享解码流程；外置卷、加密和缺少外部文件仍明确拒绝。
- 旧位置表提供 SHA-1：用 Windows BCrypt 逐文件核验，同时计算输出 SHA-256。报告增加 `sourceHashAlgorithm`，校验通过后才设置 `sourceHashVerified=true`。
- 原有 MSI、近期 Inno、路径规划、通知和输出命名保持相同使用方式。

固定结构来源：is-6_2_2，提交 `b30dac05db10c6e50391dd311e0e105cd9d05c06`。主程序及解析仍为自有 C++20；没有新增第三方解包程序或基础库。

## 用户样本复验

| 项目 | 结果 |
| --- | --- |
| 文件 | `Textify.exe`，1,686,596 字节 |
| 输入 SHA-256 | `cef497c5237f4b10b09fd4d45b051f3777f9705debb2bb6bf7dcf7ffe400b01e` |
| 内容 | Textify.exe、config.json、四个图标，共 6 个文件 |
| 文件总字节 | 236,025 |
| 包内校验 | 6 个 SHA-1 全部通过，均另算 SHA-256 |
| 结果 | `complete`；逻辑输出根 `app/` |
| 卸载程序 | 1 条安装时生成记录，仅说明，不伪造静态文件 |
| 原文件 / 安装执行 | 原输入 SHA-256 不变，没有运行 Textify 安装器或提取出的程序 |

`tests/verify-textify.ps1` 保存修复前独立读取的 6 个包内 SHA-1，用 PowerShell 再次核对实际文件与报告，避免仅以程序退出码判断成功。

```powershell
.\tests\verify-textify.ps1 `
    -Package out/dist/win-x64-release/Textify.exe `
    -OutputDirectory out/dist/win-x64-release/Textify_extracted
```

## 回归与开发工具

Debug、Release 的 CTest 三项均通过：路径保护/提交、MSI、Inno 6.7.3。另分别使用官方 6.2.2 编译器运行五种压缩 × solid 开关、中文/emoji、条件变体、空文件、别名、动态目录、dontcopy、仅密码 UI 和损坏样本验证；每种构建 63 次清单/提取调用，32 个拒绝样本均符合预期。

6.2.2 编译器仅作开发测试工具，其官方安装包 SHA-256 为 `8117d10d00a2ad33a1390978ea3872861c330e087914410a6377b22c4c5b8563`，与 [GitHub 官方发行资产](https://github.com/jrsoftware/issrc/releases/tag/is-6_2_2)记录一致。

该官方安装包另有一个运行时外部 `ISCrypt.dll` 文件记录，所以当前产品对原包整体仍返回不支持。为取得测试编译器，只在 `out/reference-tools/` 的派生测试包中去掉该外部记录，保留其余内嵌载荷不变，然后静态提取并核验。提取的 ISCC 数字签名有效。未执行官方安装器，未把原包记录为完整成功样本，未把这些开发工具放入发行目录。

6.2.2 测试无需下载旧的可选加密 DLL：真实编译仅密码 UI 的包并验证可提取，再修改派生样本的加密标志、重算元数据 CRC，确认返回“不支持加密”。真实加密生成测试仍由本机 6.7.3 执行。

## 当前产物

`out/dist/win-x64-release/Extract.exe`：0.2.1，430,080 字节，SHA-256：

```text
299a40d30453b3770c2f8b1d86c689adca696e6b9498a37c81745253ed5500aa
```

Inno 6.3/6.4 等未列出的数据标识、加密、外置卷、在线文件、NSIS 和自动递归解包仍未支持。本次仅补上用户样本暴露的精确结构及其验证，没有声明全部 Inno 6.x 已兼容。
