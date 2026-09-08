# Velopack / Squirrel 完整离线包

功能纳入 v0.8.0，见 [版本说明](releases/v0.8.0.md)。下文的本地开发状态、版本号和产物摘要保留为发布前历史验证记录。

状态：v0.7.0 发布后的本地开发，尚未发布。此模块只读取和提取文件，不启动输入安装器、更新器、安装钩子或输出程序。

## 支持范围

| 输入 | 处理方式 | 边界 |
| --- | --- | --- |
| Velopack Windows Setup EXE | PE 节内固定标记前的两个 64 位值指定 ZIP 偏移、长度；只解析该范围 | 支持已有布局及签名尾部的结构隔离；不据此认证数字签名，定制或未来布局不保证支持 |
| Squirrel.Windows Setup EXE | 静态读取命名资源类型 `DATA`、ID `131`；解开外层 ZIP，校验 `RELEASES` 后读取完整 nupkg | 必须有 `Update.exe`、`RELEASES` 和唯一根目录 `*-full.nupkg`；多个完整包暂不自动选版本 |
| 独立 `*-full.nupkg` | 校验 ZIP、根 nuspec 和 `lib/<目标>/`；整理应用文件 | 沿用完整发布包命名约定；普通 `.nupkg` / `.zip` 保持通用归档行为 |
| 差分包 | 根据 `-delta.nupkg` 和应用目录中的 `.diff`、`.bsdiff`、`.zsdiff` 标记拒绝 | 不应用补丁、不寻找或下载旧版本；改名后仍带补丁标记的内嵌包也会被拒绝 |
| 无内嵌载荷的已识别外壳 | 提示需要离线安装包或完整 nupkg | 不联网取得内容；不能识别所有厂商自定义在线启动器 |

这不是“所有 Electron 包”的统一实现。NSIS/Electron 与这两个更新框架仍走各自解析器。MSIX/APPX/Bundle 后续已由[独立模块](25-msix-appx.md)实现；差分合成或任意定制外壳仍不支持。

## 输出目录

- 单一目标：`lib/<目标>/目录/文件` → `app/目录/文件`，保留 Unicode、空格及字面百分号，不再次做 URI 解码。
- 多个目标：保存在 `app/<目标>/`，不把多个框架的同名文件合并。
- 打包元数据、`_ExecutionStub.exe` 和 Velopack 的 `Squirrel.exe` 放到 `_package`，避免把启动代理误当作应用主程序。
- Velopack 包中已有的 `sq.version` 原样保留；单目标旧布局没有该文件时，将根 nuspec 内容映射为 `app/sq.version`。
- 不展开 `resources/app.asar` 等应用数据。普通内嵌 EXE 不运行；含已知安装器标记的内层 EXE 沿用递归检查。
- Squirrel 外层 ZIP 只用于定位并验证内嵌完整包；不把外层 `Update.exe`、`RELEASES` 等启动辅助文件作为另一份应用输出。完整包自己的文件保留并逐项写入报告。

这是静态应用文件布局，**不是重建官方 portable 发布目录**：不生成 `.portable`、快捷方式、注册表或更新安装状态。应用是否可直接运行取决于它的运行库、更新框架初始化和其他安装依赖；本轮没有启动输出程序验证。

## 校验与资源处理

ZIP 读取与通用归档共用同一个实现，检查中央目录、本地头、描述符、范围重叠、路径、CRC-32 和输出 SHA-256。新增有界 ZIP 视图接口，不启动外部解包工具。

Squirrel 内嵌完整包先流式解码到受控临时文件，检查 `RELEASES` 的大小和 SHA-1，再映射其 ZIP。缓存纳入 worker 的创建记录与取消清理；大载荷不累计到内存 vector。映射仍需要连续地址空间，x86 限制见 [大包处理](17-large-packages.md)。

nuspec / RELEASES 元数据各限 4 MiB；XML 禁用 DTD，限制深度、字段长度，并拒绝重复关键字段和非 Windows 目标。声明主程序但包里缺文件时返回损坏。ZIP 原有路径、元数据及整数保护继续生效；不新增通用输入、输出体积或文件数上限。

符号链接及 `.__symlink` 占位条目暂不还原，明确报告不支持。文件名大小写冲突沿用 `_variants` 保留各份内容，不覆盖文件。

## 固定参考来源

实现由本项目编写；以下源码用于核实磁盘布局和目录语义，不将第三方解包程序链接或分发到产品。

- Velopack `1.2.0`，提交 `f2edcbcafb81da5b3c884aaea330e225ad91d8b6`：[SetupBundle.cs](https://github.com/velopack/velopack/blob/f2edcbcafb81da5b3c884aaea330e225ad91d8b6/src/vpk/Velopack.Packaging.Windows/SetupBundle.cs)、[bundle.rs](https://github.com/velopack/velopack/blob/f2edcbcafb81da5b3c884aaea330e225ad91d8b6/src/lib-rust/src/bundle.rs)、[DeltaPackageBuilder.cs](https://github.com/velopack/velopack/blob/f2edcbcafb81da5b3c884aaea330e225ad91d8b6/src/vpk/Velopack.Packaging/Compression/DeltaPackageBuilder.cs)。标记是 `SHA-256("squirrel bundle")`，不以 EXE 文件名识别生产方。
- Squirrel.Windows `2.0.1`，提交 `eef37460aef77b2f9de8cd2237c1e55b344a6554`：[WriteZipToSetup.cpp](https://github.com/Squirrel/Squirrel.Windows/blob/eef37460aef77b2f9de8cd2237c1e55b344a6554/src/WriteZipToSetup/WriteZipToSetup.cpp)、[UpdateRunner.cpp](https://github.com/Squirrel/Squirrel.Windows/blob/eef37460aef77b2f9de8cd2237c1e55b344a6554/src/Setup/UpdateRunner.cpp)、[ReleasePackage.cs](https://github.com/Squirrel/Squirrel.Windows/blob/eef37460aef77b2f9de8cd2237c1e55b344a6554/src/Squirrel/ReleasePackage.cs)。该布局是命名类型 `DATA`，并不是数值类型 `RCDATA`。
- [Velopack 1.2.0 官方发布资产](https://github.com/velopack/velopack/releases/tag/1.2.0)中的 `vpk.1.2.0.nupkg`：130,511,383 字节，SHA-256 `3e458a676be46d1122e522312db18411f36ea8c70e586f81a676695d43f89dbc`，与 GitHub 资产摘要一致。只读取其中 `vendor/setup.exe`，没有执行 packager 或模板；给该模板附加已知惰性 ZIP 并按已核实的偏移字段构造测试输入。

## 验证方法

`tests/update_package_integration.py` 默认生成 40 个静态测试用例：普通/已存在 sq.version/多框架目录、Unicode/空格/百分号、签名尾部、内层提取、普通 EXE/NuGet 不误分类、差分及缺载荷、CRC/RELEASES 错误、XML/DTD、元数据超限、路径与 PE 偏移反例。惰性 PE 没有入口点或可执行代码；只运行 Extract。

可选 `--reference-root` 增加 5 个固定样本：官方 1.2.0 外壳配已知惰性内容、官方仓库 Velopack/Squirrel 两个 LegacyTestApp 安装器、AvaloniaCrossPlat 完整 nupkg、Clowd 差分 nupkg。后四个来自上述 Velopack 固定提交的 `test/fixtures`；摘要在测试和验证 JSON 中记录。完整包逐文件与 Python ZIP 解码结果独立对照，差分包验证明确拒绝。

```powershell
python tests/update_package_integration.py `
  --executable out/build/win-x64-release/bin/Extract.exe `
  --work-root out/build/win-x64-release/update-package-fixtures
```

本阶段全量 CTest 为 12 项，后续增加 MSIX 专项后为 13 项，增加分卷专项后当前为 14 项，发布工作流要求全部通过且无跳过。官方额外样本仅本地可选验证，不自动联网下载、不随单文件程序分发；工作流仍只监听用户授权后推送的正式版本标签。

本轮 x64 / x86 的全量 CTest 各 12 项通过，无失败、无跳过，分别约 264 / 268 秒。最后仅更新帮助和版本输出的格式名称后，两个最终 EXE 再次各通过 45 个专项用例，并检查了帮助输出、内置版本、`/MT`、系统 DLL 依赖与 PE 保护标志。版本维持 0.7.0，没有创建发布标签或 Release。

本次运行数据与产物摘要见 [验证记录](data/validation-update-packages.json)。本机缺少 ARM64 MSVC 组件，本轮未构建或运行 ARM64。测试覆盖特定格式布局，不据此宣称全部近期软件兼容、干净 Windows 10/11 桌面验证或提取程序可运行。后续 MSIX/APPX/Bundle 的专用清单与块校验见 [实现记录](25-msix-appx.md)。
