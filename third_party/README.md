# 基础库管理

Extract 0.4.x 将以下 C 基础库静态链接进程序，MSVC CRT 也静态链接。运行时不需要安装编译器、Python 或解包引擎。

| 目录 | 固定版本与来源 | 使用范围 | 许可 |
| --- | --- | --- | --- |
| `lzma/` | [LZMA SDK 26.03](https://github.com/ip7z/7zip/releases/tag/26.03) | `LzmaDec`、`Lzma2Dec`、C 7z 归档读取/块解码、CRC、BCJ/BCJ2 与分支/Delta 过滤器及必需依赖 | Public domain，保留上游 `lzma-sdk.txt` 为 `LICENSE.txt` |
| `zlib/` | [zlib 1.3.2](https://zlib.net/) | inflate、CRC-32、Adler-32，定义 `Z_SOLO` | zlib，见 `LICENSE` |
| `bzip2/` | [bzip2 1.0.8](https://sourceware.org/bzip2/downloads.html) | `BZ2_bzDecompress*`；保留共享源文件的依赖子集 | bzip2，见 `LICENSE` |
| `nsis_bzip2/` | [NSIS v312 的 bzip2 基础库](https://github.com/NSIS-Dev/nsis/tree/v312/Source/bzip2) | NSIS 修改格式的流式解码；4 个源文件/头文件 | 原 bzip2 许可及 Nullsoft 修改署名，见各源文件和 `notices/NSIS-LICENSE.txt` |

下载包 SHA-256：

```text
lzma2603.7z         86c213f752520ab5325c310f50bef63ec344b56dd1c80b0246d06dc6cec953b2
zlib132.zip        e8bf55f3017aa181690990cb58a994e77885da140609fc8f94abe9b65d2cae28
bzip2-1.0.8.tar.gz  ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
```

LZMA SDK 摘要与 GitHub 官方资产记录核对，zlib 与官网摘要核对；bzip2 摘要为本次官方站点下载结果的记录。这三个目录保留的源码/头文件未修改；编译选项在本项目 CMake 目标设置。`bzip2/bzlib.c` 同时定义流式、缓冲区和文件便捷接口，产品仅调用流式解码接口，不向基础库传入输出路径。

## NSIS 格式研究与基础库适配

固定生产方源码 tag `v312`，提交 `e3f60402bcdf7be822d159b531c6e38ddf32de12`。`src/formats/nsis.cpp` 的 PE 附加区识别、CRC 范围、块/指令/字符串/语言表读取、基本块路径传播、文件映射与输出均由本项目编写；未复制安装器解释器或其它解包工具模块。

NSIS LZMA 使用已有 LZMA SDK，Deflate 使用已有 zlib 的 raw 模式。NSIS bzip2 与标准格式不兼容，因此单独保留生产方的 C 压缩基础库，编译时只启用解压部分，不引入 NSIS 主程序。它不是安装包解析模块。

`nsis_bzip2/bzlib.h` 的本地改动：去掉对安装器配置/平台头的依赖，固定解压配置，给链接符号加前缀，避免与标准 bzip2 混淆。`decompress.c` 的本地改动：增加 selector 数组上限、Huffman 索引的实际字母表边界，以及游程整数/块长度检查。改动处标注 `Extract adaptation`；`bzlib.c` 和 `huffman.c` 保持原样。`src/codecs/nsis_bzip.c` 是本项目的窄 C 接口。完整许可随便携目录中的 notices 分发。

开发测试用 NSIS 3.11 ZIP 由 [Tauri 的官方工具镜像](https://github.com/tauri-apps/binary-releases/releases/tag/nsis-3.11) 下载，SHA-256 `c7d27f780ddb6cffb4730138cd1591e841f4b7edb155856901cdf5f214394fa1` 与 [NSIS 官方发行数据](https://nsis-dev.github.io/release-data/versions.json) 一致。编译器只生成惰性测试包，不随产品分发。NSIS 3.12 使用用户提供的真实外壳样本验证，不将其来源视为官方 XnView 发行包。

MSI 数据库读取与 CAB 解压使用 Windows 系统 `msi.dll` / `cabinet.dll` API，不在此分发源码。0.4.0 已启用 LZMA SDK 的通用 C 7z 归档底层；仅传入有界字节流和受预算控制的分配器，不传入落盘路径，也不引入 `7zMain` 或外部解包进程。SFX 识别、文件选择、路径安全、临时缓存和最终输出均由本项目实现。ZIP/ZIP64 中央目录与本地记录由本项目依据 [PKWARE APPNOTE 6.3.10](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT) 解析。

新增 SDK 源码和头文件逐字节核对，未改动；编译使用 `Z7_EXTRACT_ONLY`，没有开启 PPMd。7z 固实输出映射至临时磁盘文件，元数据和辅助解码内存分别有累计限制。开发测试使用同一已核对下载包中的官方 `7zr.exe` 生成归档和独立核验真实载荷；它不随产品发行。

## Inno 格式研究与署名

0.2.1 新增的 loader v1 和 `6.1.0 (u)` 布局依据 [is-6_2_2 的 Struct.pas](https://github.com/jrsoftware/issrc/blob/is-6_2_2/Projects/Struct.pas)，固定提交 `b30dac05db10c6e50391dd311e0e105cd9d05c06`。旧包 SHA-1 校验使用 Windows BCrypt，没有新增基础库。

结构事实依据生产方 [is-6_5_4](https://github.com/jrsoftware/issrc/tree/is-6_5_4)、[is-6_6_1](https://github.com/jrsoftware/issrc/tree/is-6_6_1)、[is-6_7_3](https://github.com/jrsoftware/issrc/tree/is-6_7_3)、[is-7_1_0](https://github.com/jrsoftware/issrc/tree/is-7_1_0) 中的结构定义、序列化和压缩流代码。7.1.0 固定提交为 `7d634620f1d1caca6c76e8398da7133153db9e7b`。

`src/formats/inno.cpp` 的 CALL/JMP v3 逆变换按 `Compression.Base.pas` 规则以 C++ 重写；这是为兼容格式的实现，非上游原文件。原算法作者 Jordan Russell / Martijn Laan 的版权和许可见 `notices/Inno-Setup-LICENSE.txt`。所有元数据读取、PE 定位、路径规划与版本分支均由本项目实现，未引入上游安装器解析模块。

官方 ISCC 编译器仅用于生成开发测试包，不编入程序，也不随便携目录分发。真实第三方安装包和研究源码放在已忽略的 `out/` 中。

安装包识别、结构解析、版本适配与提取决策由本项目实现。lessmsi、innoextract、UniExtract、7z 等外部解包程序不进入运行依赖；参考源码时保留适用的许可和署名要求。
