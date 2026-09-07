# MSI 首版实现与验证

日期：2026-09-05。范围为单个内嵌独立 CAB 的 MSI 静态文件提取，不代表 MSI 全格式或全部产品路线完成。

本文件保留 0.1.0 交付时的验证记录与产物摘要。0.2.0 新增 Inno，并为统一输出增加短暂占用后的有限提交重试；当前产物与新增范围见 [Inno 报告](07-inno-implementation.md)。

## 实现流程

1. 用宽字符路径打开输入，拒绝重解析点，保留输入及其祖先目录的句柄；先检查 CFB 文件标识和大小，再调用 `MsiOpenDatabaseW` 的只读模式。
2. 读取 File、Component、Directory、Media、Summary Information 和可选 MsiFileHash。本项目关联条目并验证标识、序列、压缩标志、引用与目录环。
3. File → Component → Directory 形成逻辑目标路径。解析 `短名|长名` 与 `目标:源` 的目标部分；标准系统目录保留 `ProgramFilesFolder` 等标识，不指向本机系统目录。
4. 对大小写同名和文件/目录前缀冲突保留所有文件，稳定映射到 `_variants/file-<条目 ID 的十六进制>/<原路径>`；`_extract-report.json` 和 `_variants` 为保留名称。
5. 从 `_Streams` 读取 `Media.Cabinet` 中 `#` 指定的内部 CAB，送入 Windows FDI。回调用受控 ASCII 名称打开内存流，实际文件路径始终使用 Unicode Win32 API。
6. CAB 成员必须与 File 标识、大小和数量匹配；输出超过声明大小立即失败。存在 MsiFileHash 时调用 `MsiGetFileHashW` 核验，所有文件另算 SHA-256。
7. 写完报告后，临时目录以已打开的句柄提交为 `<包名>_extracted`，原位置已占用则尝试序号名称；批次保存任务记录并提交系统通知。

未调用安装会话、`msiexec /a`、安装动作、脚本或提取出的程序。未运行第三方解包器。Windows MSI API 负责数据库读取，FDI 负责 CAB 基础格式与压缩；安装包语义和调度为本项目实现。

依据：[只读数据库模式](https://learn.microsoft.com/en-us/windows/win32/api/msiquery/nf-msiquery-msiopendatabasew)、[File 表](https://learn.microsoft.com/en-us/windows/win32/msi/file-table)、[Media 表](https://learn.microsoft.com/en-us/windows/win32/msi/media-table)、[Directory 表](https://learn.microsoft.com/en-us/windows/win32/msi/directory-table)、[MsiFileHash 表](https://learn.microsoft.com/en-us/windows/win32/msi/msifilehash-table)、[FDICopy](https://learn.microsoft.com/en-us/windows/win32/api/fdi/nf-fdi-fdicopy)。

## 路径与资源边界

输出拒绝绝对成员路径、`..`、设备名、ADS、尾点/尾空格、非法 Unicode 和重解析目录。对目录持有 `FILE_LIST_DIRECTORY` 读取句柄且不共享写入/删除；只持有 `FILE_READ_ATTRIBUTES` 不足以实现这项保护，已加入真实 Win32 共享冲突测试。

最终提交使用 `NtSetInformationFile(FileRenameInformation)` 的单个文件名形式，在句柄原父目录内重命名，避免重新解析目标路径及 Win32 完整路径重命名对父目录的再次打开。该调用封装在 `platform::rename_in_place`，不扩散 Native API 到格式代码。[微软接口说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetinformationfile)

写入失败时仅清理本任务登记的文件和目录，不递归枚举未知内容。提交前必须释放子目录锁；此后不再写载荷或按路径清理，提交失败保留临时结果并报告位置。目录保护和预算不是进程沙箱，也不保证抵御同一用户的任意进程篡改或管理员干预。

| 限制 | 当前值 |
| --- | --- |
| 输入位置 | 本地驱动器，无重解析路径；UNC/设备路径不支持 |
| MSI 大小 | 无产品固定阈值，受 Windows Installer 格式/API 约束 |
| 内嵌 CAB 大小 | 分块写入临时文件；受 FDI 定位和实际资源约束 |
| 文件数 / 查询表行数 | 无统一数量阈值，保留文本元数据保护 |
| 总提取文件字节 | 检查实际磁盘空间及 64 位溢出 |
| FDI 回调分配预算 | 64 MiB，不是整个进程内存限制 |
| 元数据字符串累计 | 8 Mi 个 UTF-16 代码单元 |
| 目录关联深度 | 64 |

外置 CAB、多条 Media、跨卷、松散文件、管理安装映像、MST/MSP、加密/在线载荷和其它安装器不在当前范围。首版不模拟组件条件、属性重定向、DuplicateFiles、运行时生成内容，不恢复完整安装环境、权限与时间戳。输出包含 File 表静态载荷；没有 MSI 哈希的文件在报告中明确标记 `msiHashVerified=false`，计算出的 SHA-256 仅为输出摘要。

尚无 worker、硬超时、整进程内存预算及模糊测试流水线。运行平台目标是 Windows 10/11 x64，本轮只验证了当前开发机，未完成干净机器兼容矩阵。

## 自动测试

Debug 和 Release 的 CTest 均包含以下两组测试：

- `path_policy`：16 种危险名称、有效中文名称、绝对/穿越路径，目录写入及删除访问被共享锁拒绝，失败清理只移除本任务创建内容。
- `msi_integration`：自行构建包含中文文本、全部字节值、空文件和固定二进制内容的 MSI，逐文件与打包前 SHA-256 比较，同时测试存在/缺少 MsiFileHash、输入不变、中文与空格路径、同名与文件/目录冲突、重复提取、既有文件避让、批次部分失败及参数边界。

11 种异常 MSI 分别为：外部 CAB、松散文件标志、目录穿越、设备名、ADS、目录环、错误文件长度、错误 MSI 哈希、缺少内部流、损坏 CAB 标识、截断 CAB。全部必须返回预期错误、不提交输出、不遗留本次临时目录。另以真实 junction 验证输入/输出重解析路径被拒绝。

测试生成物保留在 `out/build/<预设>/test-fixtures/`，不提交安装包或载荷到 Git。自动测试不发送通知。

## 真实近期样本

本轮使用 [PuTTY 0.85 官方发行页](https://www.chiark.greenend.org.uk/~sgtatham/putty/0.85.html)中的 x64 MSI，发布时间为 2026-08-16；摘要取自其[官方 SHA-256 清单](https://the.earth.li/~sgtatham/putty/0.85/sha256sums)。

| 项目 | 结果 |
| --- | --- |
| 文件 | `putty-64bit-0.85-installer.msi` |
| MSI SHA-256 | `2213ebacf962b709411d654a40087508a694756bc4e41c5a1c24ad86596b2511` |
| 内嵌 CAB | `putty.cab` |
| 提取 | 10 文件，共 6,049,349 字节，逻辑目录 `ProgramFiles64Folder/PuTTY/` |
| 官方独立比对 | 6 个 EXE 使用官方清单的 `installer version` 项比对，帮助 CHM 也与官方摘要一致 |
| MSI 校验 | CHM、LICENCE、README、website.url 的 MsiFileHash 全部通过 |
| 输入修改 / 载荷执行 | 无 |

独立下载的 EXE 与 MSI 内的 EXE 可能摘要不同，因此不能混用官方清单的 standalone 与 `installer version` 项。复现验证只读文件：

```powershell
.\tests\verify-putty.ps1 `
    -Package .\out\samples\putty-0.85\putty-64bit-0.85-installer.msi `
    -Checksums .\out\samples\putty-0.85\sha256sums `
    -OutputDirectory .\out\samples\putty-0.85\putty-64bit-0.85-installer_extracted
```

这证明该样本的静态提取正确，不代表所有近期 MSI 或其它安装器已经支持。不会从单个产品外推覆盖率。

## 通知与发布验证范围

通知为当前用户 AUMID + 带占位 CLSID 的快捷方式 + 固定 URI 协议。无需 Windows App SDK、.NET 或常驻程序。批次记录保存于 `%LOCALAPPDATA%\Extract\jobs\<GUID>`；通知只传入 GUID，打开动作从记录中读取并重新校验本地目录。

协议探针可测试进程退出后的 Shell 激活与非法 ID 拒绝；API 提交成功仅表示系统接受请求。真实 Explorer 拖入、通知横幅出现、人工点击打开目录、免打扰行为仍需人工验收，不能由构建或协议探针替代。

本机验证记录：

- Visual Studio 18 Insiders、MSVC 19.51.36256.0、CMake 4.3.1-msvc1；Debug/Release 编译及 CTest 通过。
- Release 便携程序再次提取 PuTTY 0.85 并通过独立摘要核验；通知提交返回 `HRESULT=0`。
- 通知清理返回 0，确认快捷方式和 URI 注册消失；从便携目录重新注册返回 0，协议指向新路径，冷启动探针通过。该项验证从构建目录切换到便携目录后的注册修复，不代替真实通知点击验收。
- EXE 为 x64 Windows GUI，含 ASLR、DEP、CFG；清单为 `asInvoker`、`longPathAware=true`。只导入 Windows 系统 DLL，无独立 MSVC CRT DLL、外部解包程序或 Windows App SDK 依赖。
- 便携 EXE 与 Release 构建 EXE 完全一致，大小 280,576 字节，版本 0.1.0，SHA-256 为 `6ad5c9fe32a9462cef822dfa85a1a479bbac9d9b0c53c0272ed422d3e033d889`。
