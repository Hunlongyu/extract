# Windows 多架构发布

## 触发与授权

仅在用户明确要求“发布新版本”时，由 AI 完成版本更新、验证、提交及指定版本标签的推送。完整约束位于项目根目录 [AGENTS.md](../AGENTS.md)，后续 AI 协作应自动读取。

`.github/workflows/release.yml` 仅监听 `v数字.数字.数字` 标签的 push，并在构建前严格验证无前导零的 `vX.Y.Z` 正式版本格式及其与 CMake 项目版本的一致性。普通分支 push、PR 不运行此工作流；不提供手动或定时发布入口。

工作流无法判断是谁在聊天中授权，因此“只有用户说发布新版本才推送标签”由 AI 协作规则约束；拥有仓库写权限的人手动推送匹配标签，同样会触发工作流。

GitHub 官方说明：[标签事件过滤](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#push)、[Release 创建及 verify-tag](https://cli.github.com/manual/gh_release_create)。

## 产物与运行依赖

以 `v0.6.1` 为例，发布包含：

| 文件 | 用途 |
| --- | --- |
| `Extract-v0.6.1-windows-x86.exe` | 32 位 Windows / x86 |
| `Extract-v0.6.1-windows-x64.exe` | 64 位 Intel / AMD Windows |
| `Extract-v0.6.1-windows-arm64.exe` | ARM64 Windows |
| `SHA256SUMS.txt` | 三个 EXE 及许可文件的 SHA-256 |
| `THIRD-PARTY-NOTICES.txt` | 第三方库许可与署名，转发程序时一并保留 |

每个架构的程序是一个独立 EXE，Release CRT 使用 `/MT`，压缩基础库静态链接；不需要 VC++ Redistributable 或引擎 DLL。仍使用 Windows 自带的系统 DLL。日志、任务记录和提取目录是运行时数据，不是安装依赖。程序未配置代码签名。

x86 的可用地址空间较小，大型 7z 固实块可能无法映射；优先使用与系统原生架构对应的 x64 / ARM64 版本。超出 `size_t` 范围的 7z 块在转换前拒绝，避免 32 位截断。

## Actions 流程

1. 使用固定提交 SHA 的官方 checkout / artifact Actions。
2. 在 `windows-2025` 的 MSVC 环境分别构建 x86、x64、ARM64；通过 `vswhere` 发现工具链。
3. x86 / x64 运行全部 9 个现有 CTest，要求无失败、无跳过。NSIS 3.11 测试编译器下载后验证固定 SHA-256，Inno 编译器和 7-Zip 使用 runner 镜像预装工具，均只生成惰性测试数据。
4. ARM64 在 x64 runner 上交叉编译，关闭测试目标；目前未设置 ARM64 实机运行测试。这不等于已验证 ARM64 上的提取和通知行为。
5. `package-release.ps1` 检查全部 C/C++ 编译命令使用 `/MT`、EXE 机器类型、GUI 子系统、内置版本及普通/延迟导入的系统 DLL 白名单，再复制单文件产物并计算摘要。
6. 所有架构成功后，发布 job 才获得 `contents: write` 权限。下载并验证三个构建产物、合并许可和摘要；使用 `gh release create --verify-tag --draft` 上传。
7. 回读全部 Release 资产，核对数量及 SHA-256 后取消草稿状态。失败时保留草稿，避免把不完整版本正式发布。

不覆盖已有 Release（包括草稿），也不强制改写标签。失败重试前先检查日志和远程状态；已有草稿需要确认其内容并处理后，才能重跑。单个构建产物只保留 7 天，正式 Release 资产不受此临时保留期限影响。

runner 的 ARM64 编译组件依据 [GitHub Windows 2025 镜像清单](https://github.com/actions/runner-images/blob/main/images/windows/Windows2025-Readme.md)。镜像会更新；缺少工具、编译失败或不支持的新测试数据都会阻止发布。

## 本地构建与预检查（不会发布）

```powershell
./scripts/build.ps1 -Architecture x86 -Configuration Release -Test
./scripts/build.ps1 -Architecture x64 -Configuration Release -Test
./scripts/build.ps1 -Architecture arm64 -Configuration Release -WithoutTests

# 以下标签只是本地版本校验参数，不会创建 Git 标签；必须与当前 CMake 版本一致。
./scripts/package-release.ps1 -Architecture x64 -Tag v0.6.0
```

构建输出位于 `out/build/win-<架构>-release/bin/Extract.exe`。加 `-Install` 可生成含双语 README、logo 和许可的便携目录；与 GitHub 的单 EXE 下载方式兼容。

具备三个架构的产物后，可把三个 EXE 及对应 `.sha256` 文件放在同一目录，执行：

```powershell
./scripts/publish-release.ps1 -Tag v0.6.0 -AssetDirectory out/release-assets -PrepareOnly
```

`-PrepareOnly` 只做本地校验和整理。没有此参数时，脚本要求标签 push 的 GitHub Actions 环境，不接受普通本地调用。

## 验证边界

配置阶段不创建或推送发布标签，不创建 Release。首次实际云端构建和发布应在用户下一次明确要求“发布新版本”后执行，不能以配置完成代替云端验证。

2026-09-07 本地验证：x86 / x64 Release 各 8 项 CTest 全部通过；新增超 4 GiB 的 7z 元数据用例只执行列目录，确认 x86 返回 223、x64 正常列出，未展开巨型数据。两架构通过版本、PE 和静态 CRT/依赖检查；actionlint 1.7.12 通过。发布脚本用合成数据验证了摘要合并、篡改拒绝及本地发布拦截，未调用发布 API。本机未安装 MSVC ARM64 组件，因此 ARM64 编译及运行尚未在本地验证。
