# Metasequoia IME Linux 完整版设计

## 目标与“完整”的定义

Linux 版以 Windows 版的用户能力为产品基准，但不复制 TSF、Named Pipe、WebView2 或 Win32 窗口实现。完整性按用户可观察行为判断：中文全拼、双拼、五笔和日文输入可切换；中英文、标点、全半角、候选选择和翻页行为稳定；配置与用户词频可持久化；英文、Emoji、颜文字、日期时间、Unicode、快捷短语、临时英文/日文等模式可用；网络候选、AI、翻译和语音能力失败时不影响本地输入；设置、安装、升级、诊断和发行包达到日常可用状态。

Linux 首个前端继续使用 IBus。候选窗、预编辑和语言栏属性使用 IBus 原生协议，不要求复刻 Windows 的像素级界面。手写、屏幕键盘、悬浮工具栏和剪贴板面板属于桌面扩展层，在核心输入稳定后以独立进程实现。这样既保留 Windows 的功能语义，也符合 Linux 桌面的生命周期、安全和发行习惯。

验收采用功能矩阵而非 README 宣称。每项功能必须至少有一种权威证据：共享 core 单元测试、IBus 适配器测试、真实 IBus 会话冒烟、配置迁移测试、服务集成测试或打包安装测试。只有矩阵中的必需项全部有证据时，才称为“完整版”。

## 架构与组件边界

`MetasequoiaImeEngine` 保持平台中立，拥有输入方案、分词、候选生成、动态候选缓存和用户词典。它公开 `InputSession` 作为原生前端 API；方案切换、辅助码和自动纠错等所有前端都会使用的控制能力应进入这里。Engine 不依赖 IBus、GLib、桌面窗口或 Linux 配置路径。

Linux 仓新增可测试的 `InputController`，拥有中英文状态、当前候选光标、页大小、按键到 Engine 命令的映射及“先提交再切换”的事务语义。控制器使用简单 C++ 值类型作为输入输出，不包含 IBus 类型，因此所有核心键盘行为可在 macOS 开发机和 Ubuntu CI 中测试。

`IBusEngine.cpp` 仅做协议适配：把 IBus keyval/state 转为控制器事件，把控制器快照发布成 preedit 和 lookup table，把 property 激活转为模式变更。候选表光标由控制器持有，IBus 的键盘回调、面板翻页回调和鼠标点击都走同一状态机，避免多套索引算法漂移。

`SettingsStore` 使用 GLib 的 key-file API，把非敏感配置写到 XDG config 目录；数据库和用户数据使用 XDG data 目录。在线服务和凭据后续放入独立 service 层，密钥优先走 Secret Service，绝不写日志。候选异步结果携带 composition generation，过期结果必须丢弃。

## 数据流、错误处理与验证

按键数据流为：IBus 事件 → Linux `InputController` → Engine `InputSession` → 不可变视图快照 → IBus preedit/lookup table。提交结果单独返回，适配器先提交文本再刷新 UI。模式或方案切换若发生在 composition 中，控制器先提交当前高亮候选，再重置 Engine 并改变模式，保证输入不丢失。带 Ctrl、Alt、Super 的宿主快捷键在提交现有 composition 后继续传给应用。

本地词库缺失、损坏或版本不兼容时，Engine 初始化应返回可诊断错误，IBus 进程保持存活并显示简短 auxiliary message。配置解析采用逐字段默认值和范围校验；未知字段保留，旧版本配置通过幂等迁移升级。网络、AI、翻译和语音服务统一采用取消、超时和 generation 校验，本地候选永远不等待网络。

验证分三层：纯 C++ 测试覆盖方案切换、候选光标/翻页、数字选择、模式切换和标点；Ubuntu 24.04 CI 构建真实 IBus 二进制并运行 CTest；容器或虚拟机中的 `ibus-daemon` 冒烟覆盖组件注册、引擎创建和 D-Bus 方法调用。发行阶段再增加 Debian 包安装/卸载及 GNOME/KDE 人工兼容矩阵。

## 交付阶段

1. **桌面核心**：全拼/双拼/五笔/日文、中英文模式、候选光标与翻页、IBus properties、XDG 配置。
2. **中文输入体验**：中英文标点、全半角、成对/智能标点、以词定字、preedit 样式、用户调频和辅助码。
3. **本地扩展模式**：英文混输与独立英文、Emoji、颜文字、日期时间、Unicode、快捷短语、超级简拼、临时英文/日文。
4. **在线能力**：云候选、AI 联想、候选翻译和统一 provider 配置；所有调用异步且可降级。
5. **桌面工具与发行**：设置应用、Secret Service、语音、剪贴板、手写、屏幕键盘、悬浮工具栏、Deb/RPM/Arch 包、升级迁移和诊断。

每一阶段都必须保持前一阶段可安装、可输入、测试全绿。Windows 新增跨平台功能时先判断归属：算法和状态机进入共享 Engine，桌面协议和窗口生命周期留在各自前端。
