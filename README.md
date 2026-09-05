# Extract

Windows 原生安装包解包工具，优先覆盖近几年发布的安装包。

技术栈：**C++20 自研代码 + C 基础库 + CMake + Win32**。C 编译标准设为 C17，不为混合语言而刻意拆分自研模块。

更新日期：2026-09-05。当前为 **0.1.0 工程骨架**：已建立构建配置、宽字符 Win32 入口、版本资源和便携目录生成流程。**安装包解析、文件提取、工作进程和系统通知尚未实现，当前不能用于解包。**

## 构建

要求 Windows、Visual Studio C++ 桌面开发工具（支持预览版）、CMake 3.25+ 和 Ninja。脚本自动发现 Visual Studio 并初始化 x64 MSVC 环境，优先使用其自带的 CMake/Ninja。

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release -Install
```

- 编译产物：`out/build/win-x64-release/bin/Extract.exe`
- 便携目录：`out/dist/win-x64-release/`
- 详细步骤与当前行为：[构建与开发](docs/05-build-and-development.md)

工程骨架支持 `--help`、`--version`。传入安装包路径只返回“不支持”（退出码 50），不读取或修改输入，也不创建结果目录。当前诊断写入已有控制台、重定向输出或调试器；从 Explorer 启动暂时没有可见提示。

## 已确认的要求

- 主程序、安装包识别、格式解析与提取流程由本项目实现。
- 允许使用原生 C/C++ 压缩与归档基础库；无需自行发明 ZIP、LZMA 等算法。
- 参考 UniExtract、UniExtract 2、lessmsi 的设计和源码，不以调用这些程序作为产品实现。
- 无主界面，文件拖到程序图标上即可处理；成功失败用右下角系统通知。
- 便携文件夹发布；保留原生程序、必要基础库和说明文件。
- 优先近期安装包，不为老旧安装器维护大量历史兼容分支。

## 推荐范围

“近几年”暂按 2021–2026 年发布的软件安装包建立样本集，以当前稳定版本为重点，不按文件时间戳拒绝输入。

核心阶段：Inno Setup 6.2–6.7 与 7.0/7.1、NSIS 3.x 和近期 Electron/NSIS 封装、MSI；配套实现 ZIP/7z/CAB 读取。扩展阶段：MSIX/APPX、WiX Burn、Velopack/Squirrel 的近期离线包。

以上是开发目标，不是已验证支持列表。模块按实际二进制结构和样本验收，不能只凭产品版本号承诺全部变体。

## 文档

| 文档 | 内容 |
| --- | --- |
| [01 可行性与技术选型](docs/01-feasibility.md) | 自主实现边界、近期格式、参考来源、基础库选择 |
| [02 产品需求](docs/02-requirements.md) | 已确认需求、拖拽与输出、通知、兼容边界 |
| [03 技术设计](docs/03-technical-design.md) | 原生模块、统一文件模型、MSI/Inno/NSIS 解析、嵌套载荷 |
| [04 开发计划与验收](docs/04-roadmap-and-acceptance.md) | 按格式交付、样本矩阵、近期覆盖率和工期估算 |
| [05 构建与开发](docs/05-build-and-development.md) | 工具链、构建命令、目录、初始化范围和验证方法 |

先完成 MSI/CAB 的原生闭环，同时验证 Inno 7 结构，再逐步加入 Inno 和 NSIS。参考源码可以降低格式研究成本，剩余工作主要是解析实现、版本适配与真实包验证。工作量按模块估算，详见开发计划。
