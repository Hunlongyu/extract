# NSIS ANSI 兼容性与 QQ 音乐样本

This change adds selected NSIS 2.x ANSI layouts alongside the existing Unicode 3.x support. It statically extracts embedded files without running the installer. ANSI 3.x and arbitrary modified opcode layouts remain outside this change.

## 原因与修复

QQ 音乐样本使用 1,048 字节的 ANSI 区段记录，原解析器只接受 2,072 / 16,408 字节的 Unicode 记录，因此在读文件路径之前返回“不支持”。软件版本较新，不代表它的安装器外壳也使用新版 Unicode 格式。

本次新增 ANSI 1,024 / 8,192 字符区段布局读取，保留字符串表中的字节索引。按 NSIS 2.x 的 `FC` 字面转义、`FD` 变量、`FE` Shell 目录、`FF` 语言引用解码，先还原字面字节，再转换为 Unicode。这样不会将中文双字节字符的后半字节误当成路径分隔符或转义指令。

代码页从包内语言表推定，QQ 音乐样本为 936。多个语言的代码页不一致、语言标识无效或没有有效代码页时，只接受 ASCII 路径；不使用当前 Windows 的默认代码页代替。推定代码页不等于获得了原编译时的完整编码配置，因此混用编码的修改版仍可能不兼容。

CRC、指令和字符串边界、解码长度、路径穿越防护、临时缓存清理及有限目录传播继续生效。ANSI 的 `01/03 + 编码引用` 会被作为未支持的 3.x 编码拒绝，不能直接套用 2.x 规则。默认品牌字符串缺失时只报告兼容布局，不声称确认了编译器补丁版本。

依据生产方固定 [v251/fileform.h](https://github.com/kichik/nsis/blob/v251/Source/exehead/fileform.h)、[v251/build.cpp](https://github.com/kichik/nsis/blob/v251/Source/build.cpp)、[v251/util.c](https://github.com/kichik/nsis/blob/v251/Source/exehead/util.c) 及 [v311/build.cpp](https://github.com/kichik/nsis/blob/v311/Source/build.cpp) 核对结构和编码。没有引入外部解包工具或运行时 DLL。

## 真实样本验证

| 项目 | 结果 |
| --- | --- |
| 输入 | `QQMusic_22.10.0.0_mefcl_Setup.exe` |
| 大小 | 74,514,819 字节 |
| SHA-256 | `29d0dcc50680c16e04cc555d8873e49db1c0b945958c7651fb5295123d3e306d` |
| 格式 | NSIS 2.x ANSI 兼容布局，LZMA Solid |
| 结果 | `complete`，268 个文件，220,640,492 字节 |
| 应用目录 | `app` 下 241 个文件，含 `QQMusic.exe` |
| 安装器资源 | `plugins` 下 27 个文件 |
| 校验 | 包内 CRC32 通过；全部输出与独立解压的原始载荷逐字节一致 |

独立验证使用 Python LZMA 解压固实流，读取文件指令的载荷偏移并逐文件比较，同时重新计算输出 SHA-256。NSIS 没有包内逐文件摘要，不将输出 SHA-256 称为生产方签名或摘要。没有执行 QQ 音乐安装器、插件、脚本或应用；提取成功不等于运行能力已验证。

## 自动测试与限制

`tests/nsis_integration.py` 保留官方 3.11 Unicode 的三种压缩、普通 / Solid、架构目录及损坏数据测试，并用官方 3.11 生成不压缩 ANSI 惰性包，再独立转换为已核对的 2.x 字符串编码作为派生样本。它不是官方 2.51 编译器生成包，不声称完成该编译器的全矩阵验证。

新增回归覆盖语言引用、变量目录、ASCII、CP936 转义双字节字符、CP1252 西欧字符、未知代码页、无效编码、路径穿越、截断转义和过长字符串。8,192 字符 ANSI 区段按已知结构支持，尚无对应官方编译器样本验收。

本轮 x86 / x64 各 121 次 NSIS 调用通过；x64 的 20 个嵌套场景及两个架构的路径、进度测试通过。两个最终便携程序重新提取 QQ 音乐，全部 268 个文件的路径与内容摘要一致。运行结果和本地产物摘要见 [验证记录](data/validation-nsis-ansi.json)。本地更新不创建版本标签或 GitHub Release。
