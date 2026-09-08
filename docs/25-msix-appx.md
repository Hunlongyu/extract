# MSIX / APPX / Bundle 静态提取

功能纳入 v0.8.1，见 [版本说明](releases/v0.8.1.md)。下文的本地开发状态、版本号和产物摘要保留为发布前历史验证记录。

日期：2026-09-07。这是 v0.7.0 发布后的本地开发改动，尚未发布。安装包解析由本项目 C++20 实现，使用已有 ZIP/Deflate 基础层、系统 XmlLite 和 CNG；不调用 MakeAppx、MSIX SDK 或第三方解包工具完成产品解包。

## 支持范围

| 输入 | 行为 |
| --- | --- |
| 未加密 `.msix` / `.appx` | 读取包身份、版本、架构、资源/框架属性、应用路径和依赖说明；按块校验内容，保留原始目录 |
| 完整 `.msixbundle` / `.appxbundle` | 交叉核对内嵌包身份、版本、架构、ResourceId、类型及 ZIP 偏移/长度，展开所有内嵌成员 |
| 资源包 | 保留清单、语言/缩放资源及文件；不把它当成独立可运行应用 |
| 改名为 `.zip` 的包 | 根据根部 Appx 元数据切换到专用解析和校验，不绕过块检查 |
| Flat Bundle 缺少外置成员 | 保留已有内容，返回 299 部分完成；记录缺失文件，不读取包外成员或下载 |
| Bundle `IsStub` 成员 | 提取已有内容并标为部分完成，不声称得到完整应用 |
| 加密包、未知块算法/布局、Optional 包关系 | 明确返回不支持（50） |
| 文件缺失、偏移/身份不符、哈希损坏 | 单包失败（13）；内嵌包提取失败时保留已提交外层，返回部分完成（299） |

AppxManifest 支持 Windows 10 foundation、appx/2010 和 appx/2013 根命名空间；Bundle 为 appx/2013/bundle。BlockMap 为 appx/2010/blockmap，接受 SHA-256、SHA-384、SHA-512，以及 2017/2021 命名空间的可选 FileHash。识别这些命名空间不代表覆盖全部扩展或完成部署 XSD 验证。

## 目录与结果

单包保留 `AppxManifest.xml`、`AppxBlockMap.xml`、`[Content_Types].xml`、资源、签名文件（若有）和 `VFS` 等原始内容。OPC 百分号编码按 UTF-8 解码一次，中文、空格和百分号恢复为实际文件名；禁止编码后的分隔符、路径穿越、重解析点、解码重名和文件/目录冲突。

Bundle 布局示例：

```text
Example_extracted/
  AppxMetadata/AppxBundleManifest.xml
  packages/
    application/x64/Example-x64.msix
    application/x64/Example-x64_extracted/...
    application/x86/Example-x86.msix
    application/x86/Example-x86_extracted/...
    application/arm64/Example-arm64.msix
    application/arm64/Example-arm64_extracted/...
    resource/neutral/Example-zh.msix
    resource/neutral/Example-zh_extracted/...
  _extract-report.json
```

原始成员包保留，所有架构分别展开，不按当前操作系统筛选或合并文件。清单中的语言、缩放和设备依赖属性记录到成员 `conditions`；完整 XML 也会保留。报告包含 `blockHashAlgorithm`、`blockHashVerified` 和递归成员状态。`--list` 只验证结构及读取到的元数据 CRC，不声称校验完成全部载荷块；实际提取才将对应标记置为 true。

## 校验和资源处理

1. 自有 ZIP/ZIP64 解析器读取中央目录、数据描述符与本地头；支持 MakeAppx 的全哨兵 EOCD，真实跨磁盘归档仍拒绝。
2. 有界读取 XML，禁用 DTD/外部实体。单份 XML 最多 16 MiB、500,000 节点、64 层；无命名空间属性最多 128 个，单属性值最多 32,768 字符。这是元数据预算，不是输入包统一体积限制。
3. 检查块映射 File 大小、本地头长度、块数量、Base64 与压缩长度。Deflate 允许官方打包器最后追加的 `03 00` 结束标记。
4. 解码时每 64 KiB 未压缩内容计算块哈希，末块按实际长度；同时验证 ZIP CRC 和可选整文件 FileHash。校验失败不提交该包的输出目录。
5. Bundle 外层块映射仅覆盖 Bundle 清单；成员必须以 ZIP stored 保存，成员内容由递归单包处理器独立校验。内层失败会留下外层原始包供排查，并明确报告部分完成。

保持现有进度、取消、工作进程隔离和临时目录清理；块解码与哈希处有取消检查。继续沿用 [大包资源约束](17-large-packages.md)和全树 4 层/32 包的递归保护。大型 Bundle 可能因成员数量保护而部分完成，不宣称无限大小或无限嵌套。

## 验证方法

`tests/msix_integration.py` 使用 Windows SDK MakeAppx 生成惰性样本，并由 Python 标准库独立读取 ZIP，逐文件与打包前内容比较。测试中的 EXE 文件只是文本数据，从不启动。单包 `pack /nv` 跳过应用部署语义验证，以允许惰性内容；Bundle 使用官方 `bundle` 打包。它们证明包结构及提取行为，不证明样本是可部署、可运行的应用。

专项覆盖官方压缩/不压缩包、SHA-256/384/512、x86/x64/ARM64 内容、neutral 资源包、四成员 Bundle、中文/空格/百分号路径、跨 64 KiB 块及空文件；同时保留块和 FileHash 损坏、大小/偏移/身份错误、缺失/重复成员、外置成员、占位包、加密、DTD、错误命名空间、路径攻击等反例。提取 ARM64 内容不等于运行 ARM64 版 Extract。

运行结果、完整用例及最终产物摘要见 [验证数据](data/validation-msix-appx.json)。该阶段全量 CTest 注册为 13 项（后续增加 [分卷专项](26-split-volumes.md) 后当前为 14 项），x86/x64 发布构建要求无失败、无跳过；缺少 SDK MakeAppx 时专项返回 77。本轮也修正中文 MSVC 的头文件依赖识别，并对受影响旧目标执行干净构建。

本轮实际使用 MakeAppx 10.0.26100.8249；x64 / x86 全量 CTest 各 13 项通过，无失败、无跳过，用时约 328 / 310 秒。每个架构专项各 50 例：12 例完整提取、3 例预期部分完成、35 例预期拒绝，均返回预期结果。最终便携 EXE 与测试二进制摘要一致，静态 CRT、系统 DLL 依赖、ASLR/DEP/CFG 和 `--list` 不虚报块校验均通过核对。版本保持 0.7.0，没有创建标签或发布版本。

## 当前边界

- 不部署、注册、执行程序或脚本，不补注册表、服务、包身份或依赖；依赖安装环境的应用未必能直接运行。
- 块哈希和 CRC 是一致性校验，不是来源认证；不验证 AppxSignature 的发布者信任链。修改内容并重建块映射不等于可信包。
- `[Content_Types].xml` 检查安全 XML 和根结构，不执行完整 OPC 内容类型及部署模式验证。
- 不支持加密包、Optional 包完整关系、外部成员还原/下载、差分应用和资源自动合并。未知块映射扩展明确拒绝。
- 本轮使用官方打包器惰性样本和结构反例；近期第三方真实应用包、干净 Windows 10/11 桌面与 ARM64 实机仍需扩展验证。

## 格式依据

- [Microsoft：MakeAppx 打包工具](https://learn.microsoft.com/en-us/windows/msix/package/create-app-package-with-makeappx-tool)
- [AppxManifest](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/appx-package-manifest)、[BlockMap File](https://learn.microsoft.com/en-us/uwp/schemas/blockmapschema/element-file)、[Block](https://learn.microsoft.com/en-us/uwp/schemas/blockmapschema/element-block)、[Bundle Package](https://learn.microsoft.com/en-us/uwp/schemas/bundlemanifestschema/element-package)
- 已核对的 Microsoft SDK 提交为 [`25a65f5c1690930813bcc10cdf1d59fa865f2bb1`](https://github.com/microsoft/msix-packaging/tree/25a65f5c1690930813bcc10cdf1d59fa865f2bb1)，包括 BlockMap、Bundle、ZIP 和 OPC 编码规则；未引入其解析代码或运行库。
