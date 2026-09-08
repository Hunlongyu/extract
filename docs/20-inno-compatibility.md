# Inno compatibility / Inno 兼容性

Standard embedded packages are supported for the exact data layouts below. Compiler versions and data layout identifiers are different. Modified repacks, encrypted payloads and downloaded files are not covered by this table. External split volumes are now implemented locally; their separate verification matrix and limitations are documented in [split packages](26-split-volumes.md).

## 精确布局与验证范围

新增标准布局按照官方源码分别适配，不根据版本号接近程度猜测结构。表中“本轮”指本地工作区验证，不表示已经发布新版本。

| 数据标识 | 官方编译器样本 | 验证记录 |
| --- | --- | --- |
| `6.0.0 (u)` | 6.0.5 | 本轮新增、x86 / x64 Release |
| `6.1.0 (u)` | 6.2.2 | 原有支持，本轮回归 |
| `6.3.0` | 6.3.3 | 本轮新增、x86 / x64 Release |
| `6.4.0.1` | 6.4.1 | 本轮新增、x86 / x64 Release |
| `6.4.2` | 6.4.2 | 本轮新增、x86 / x64 Release |
| `6.4.3` | 6.4.3 | 本轮新增、x86 / x64 Release |
| `6.5.0` | 6.5.0 | [嵌套提取阶段](10-nested-extraction.md) |
| `6.5.2` | 6.5.4 | [初始验证](07-inno-implementation.md) |
| `6.6.0` | 6.6.0 | 本轮新增、x86 / x64 Release |
| `6.6.1` | 6.6.1 | [初始验证](07-inno-implementation.md) |
| `6.7.0` | 6.7.3 | 原有支持，本轮回归 |
| `7.0.0.3` | 7.1.0 | [初始验证](07-inno-implementation.md)，含 x64 安装器 |

同一编译器系列可能改变数据布局；此表不承诺全部 6.x / 7.x、ANSI 或第三方修改版兼容。表中 x86 / x64 Release 表示 Extract 的运行架构。

## 实现变化

- 6.0 复用经核对的 Unicode 文件表结构；6.3 增加架构表达式字符串、调整头选项长度及独立签名选项。
- 6.4 适配密码描述、SHA-256、头部字符串数量以及文件位置标志的多次变化。
- 6.6.0 与 6.6.1 分别处理向导图像字段差异。
- 元数据 CRC、文件 SHA-1 / SHA-256、路径保护、长度和整数检查继续生效；加密与缺少外部载荷明确拒绝。
- 分卷参数读到异常值时同时提示可能是修改版头部不匹配，避免将此情况直接断言为多分卷。

结构依据来自固定官方 tag 的 [6.0.5 Struct.pas](https://github.com/jrsoftware/issrc/blob/is-6_0_5/Projects/Struct.pas)、[6.3.3 Struct.pas](https://github.com/jrsoftware/issrc/blob/is-6_3_3/Projects/Src/Struct.pas)、[6.4.1 Shared.Struct.pas](https://github.com/jrsoftware/issrc/blob/is-6_4_1/Projects/Src/Shared.Struct.pas)、[6.4.2](https://github.com/jrsoftware/issrc/blob/is-6_4_2/Projects/Src/Shared.Struct.pas)、[6.4.3](https://github.com/jrsoftware/issrc/blob/is-6_4_3/Projects/Src/Shared.Struct.pas) 和 [6.6.0](https://github.com/jrsoftware/issrc/blob/is-6_6_0/Projects/Src/Shared.Struct.pas)。既有布局来源见初始验证文档。

## 回归方法

`tests/inno_integration.py` 可指定以上官方编译器，生成只有已知测试内容的安装包。仅运行编译器和 Extract，不执行安装器、内嵌脚本或提取出的应用。

```powershell
python tests/inno_integration.py `
    --executable out/build/win-x64-release/bin/Extract.exe `
    --compiler C:/path/to/official/ISCC.exe `
    --work-root out/inno-test
```

本轮覆盖 8 个官方编译器：6.0.5、6.2.2、6.3.3、6.4.1、6.4.2、6.4.3、6.6.0、6.7.3。每个版本覆盖五种压缩方式与 solid 开关、中文 / emoji、空文件、别名、条件同名文件、动态目录、清单与提取一致性、包内哈希和原始字节核对，以及损坏、加密、外置文件、未知标志、路径穿越等拒绝样本。旧布局增加异常头部与独立签名枚举测试。

x86 / x64 Release 共 16 组矩阵、1,004 次 Extract 调用全部通过；两架构的 MSI、路径保护、进度三项 CTest 也通过。逐架构结果见 [验证摘要](data/validation-inno-compatibility.json)。默认 CTest 仍使用本机官方编译器；其余版本通过上述参数单独运行。

开发编译器从官方发行资产静态提取，安装包 SHA-256 与官方资产摘要核对，提取后的 ISCC 签名有效。6.2.2 / 6.3.3 官方安装包包含外部 `ISCrypt.dll` 记录：只在开发派生包中移除这条记录并清除失效的容器签名目录，然后重算元数据 CRC、保留原内嵌文件及其哈希。原安装包仍按外部文件限制拒绝，不能记为完整提取成功。旧版测试使用密码界面包与加密标志变异样本；6.4 起另外生成实际加密样本。编译器不进入发行目录。

## Ant Download Manager 重打包样本

`Ant.Download.Manager.Pro.v2.16.7.RePack.by.xetrin.exe`，99,724,306 字节，SHA-256：

```text
ccfa1851400f8ecefb151cbeb1281c8ddc92ad11cdfea10b11f229972ab1d98b
```

该样本仍然返回“不支持”，没有提取成功：

- 标识为 `Inno Setup Setup Data (6.0.0) (u)`，loader 和元数据 CRC 正确。
- 头部不符合官方 6.0.0 Unicode 布局；按标准字段位置读取分卷参数得到 256，而正常样本为 1。
- 位置表长度对应 913 条旧式记录，其中 905 条设置了标准旧布局中的加密位。这是进一步适配前需要确认的证据，不能仅凭标志断言其加密算法与官方完全相同。
- 用户确认没有提供密码。没有尝试执行安装脚本来取得密码，也没有跳过完整性检查冒充成功。

因此新增标准版本支持不能直接解决此重打包。后续需要单独确认修改版头部、载荷加密方式及密钥来源；不能仅放开版本检查或猜测偏移。
