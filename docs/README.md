# Extract 技术文档总览

欢迎查阅 Extract 的技术设计与开发文档库。本项目是一款轻量、无主界面、通过拖拽解包常见 Windows 安装包的独立 C++ 原生工具。

本文档汇总了项目的架构设计、核心机制、安装包格式解析专题以及构建发布指南。

---

## 快速导航

### 1. 架构设计与核心机制

| 文档 | 说明 |
| :--- | :--- |
| [requirements.md](requirements.md) | 产品功能定义、核心使用体验与优先级边界 |
| [technical-design.md](technical-design.md) | 系统整体分层架构、流式管道、错误隔离与安全模型 |
| [logging.md](logging.md) | 日志轮转、脱敏规则、回退路径与故障排查 |
| [large-packages.md](large-packages.md) | 超大安装包支持、内存与磁盘资源防护、连续映射约束 |
| [progress-notifications.md](progress-notifications.md) | Windows 原生系统通知、实时字节进度与任务交互规范 |
| [worker-cancellation.md](worker-cancellation.md) | 独立工作进程沙箱、异常隔离、单包超时与中断恢复 |
| [compact-layout.md](compact-layout.md) | 智能精简输出布局、应用主程序根提升与元数据归集规则 |

### 2. 安装包格式解析专题

Extract 采用纯自研解析引擎，直接静态读取二进制安装包结构，不依赖任何外部解包工具或第三方执行环境：

| 格式分类 | 专题文档 | 支持要点与覆盖范围 |
| :--- | :--- | :--- |
| **Inno Setup** | [inno-compatibility.md](inno-compatibility.md)<br>[split-volumes.md](split-volumes.md) | 适配 6.0–6.7、7.1 标准结构族，外置分卷（`.bin` 卷链），跳过安装代码，提取编译脚本记录与原始哈希 |
| **NSIS** | [nsis-shell-directories.md](nsis-shell-directories.md)<br>[nsis-ansi-compatibility.md](nsis-ansi-compatibility.md) | Unicode 3.x 与 ANSI 2.x 布局，解析虚拟外壳目录，导出反编译指令 `nsis-script.txt`，多架构分支识别 |
| **Windows Installer & Burn** | [split-volumes.md](split-volumes.md) | MSI 内嵌/外置/混合 CAB 卷链与跨卷文件，WiX Burn 布局 2 容器，松散源文件与长短文件名关联 |
| **现代应用包** | [msix-appx.md](msix-appx.md) | MSIX / APPX / Bundle 离线解包，块哈希校验，分架构与多资源包展开 |
| **离线更新包** | [update-packages.md](update-packages.md) | Velopack 与 Squirrel.Windows 完整离线包识别，应用文件自动归集与版本分支整理 |
| **基础归档与 SFX** | [technical-design.md](technical-design.md) | ZIP/ZIP64、7z、CAB 容器及 PE 附加区自解压封装（SFX）的通用静态读取 |

### 3. 构建、开发与运维

| 文档 | 说明 |
| :--- | :--- |
| [build-and-development.md](build-and-development.md) | 编译器要求、CMake 预设、构建脚本、完整命令行参数与退出码 |
| [release.md](release.md) | 多架构发布规范、GitHub Actions 自动化流水线与标签授权原则 |
| [roadmap-and-acceptance.md](roadmap-and-acceptance.md) | 项目里程碑规划、开发路线图与当前验收状态 |
| [releases/](releases/) | 历次正式版本的详细更新日志与发布说明 |

---

## 推荐阅读路径

- **新加入开发者**：建议先阅读 [build-and-development.md](build-and-development.md) 配置本地 MSVC 与 CMake 环境并执行构建；随后阅读 [technical-design.md](technical-design.md) 了解整体模块划分。
- **扩展格式支持**：如需增强或新增安装包格式，建议依次参考 [inno-compatibility.md](inno-compatibility.md) 与 [nsis-shell-directories.md](nsis-shell-directories.md)，了解项目如何通过只读方式解析二进制文件结构。
- **排查解包异常或崩溃**：请先阅读 [logging.md](logging.md) 获取运行日志位置与排查方法，并阅读 [worker-cancellation.md](worker-cancellation.md) 理解子进程隔离逻辑。

---

## 核心设计原则

1. **绝对静态安全**：只按字节解析数据格式，绝不调用目标程序的安装动作，不执行内嵌的脚本代码，不启动释放出来的任何二进制可执行文件。
2. **纯原生与零外部依赖**：自研代码使用现代 C++20，底层算法仅链接经审计的静态 C 库，编译为完全自包含的单一便携 EXE，无需安装 VC++ 运行库。
3. **有界的资源管理**：无人工强加的固定包大小限制，但严格实施整数溢出保护、只读流式映射与目录遍历防护，杜绝恶意包导致的路径穿越或系统目录破坏。
