# MSIME-Linux

组织职责见 [组织规范](https://github.com/metasequoiaime/.github/blob/main/AGENTS.md)。Linux 负责 IBus、GTK 和系统集成；输入算法由固定 Engine 的公共 Session 动作与快照提供，辅助码来自同一 gitlink 下的 helpcode/helpcodes，不再单独检出 HelpCode。发布词库只通过 product-lock.json 消费 Engine 的公开词库产物，禁止在平台复制建库/分表/压缩算法。公共词库验证器的副本必须与 Engine gitlink 完全一致。

本仓默认分支是 `develop`，日常改动从 `develop` 切分支并合回 `develop`；`main` 是发布分支，只在发版时由维护者从 `develop` 合入，`release.yml` 也只监听 `main`。特性分支直接提到 `main` 会被 `Branch guard` 拦下。规则见[组织 AGENTS.md 的分支模型](https://github.com/metasequoiaime/.github/blob/main/AGENTS.md#分支模型)。

InputController 在每次输入变更后保存值快照，界面读取不得再次维护组合状态机。
高亮候选结束组合调用 Session::finish(index)，剩余分段仍由 Engine 提交。
本地模式和候选开关是构造时已验证的产品选项；独立英文模式状态读取 Engine 快照。
按输入方案切换辅助码开关与码表。启动先捕获 RuntimePaths::legacy() 作为稳定协调根，
取得共享租约后解析活动代际；只有没有活动记录时才使用旧布局。

IBus 在输入上下文创建时捕获 RuntimePaths，并把同一布局传给控制器、词库可用性检查和翻译查询。
资源准备完成后可通过 InputController 的显式 RuntimePaths 构造入口接入；控制器不得重新解析环境变量。
资源准备调用安装后的固定验证器；运行中切换通过 app.msime.Dictionary 协调所有 IBus 上下文。
有组合输入时不得切换；独占租约内重检预览 token/本机摘要，并先构建替代会话再发布。
发布成功即采用新路径，即使目录 fsync 未确认也不得回滚或重复提交旧预览。
设置界面通过 NativeRestoreReview 下载并冻结完整快照、验证资源和准备代际；显示账号、记录数量与替换范围后才允许发布。
预览绑定账号代次及 IBus 唯一总线 owner；取消只清理未尝试发布且非活动的自有代际。
发布开始后等待确定回复，不因关闭窗口撤回；结果不确定时保留代际，不能把资源准备成功视为恢复完成。
