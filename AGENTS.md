# MSIME-Linux

组织职责见 [组织规范](https://github.com/metasequoiaime/.github/blob/main/AGENTS.md)。Linux 负责 IBus、GTK 和系统集成；输入算法由固定 Engine 的公共 Session 动作与快照提供，辅助码来自同一 gitlink 下的 helpcode/helpcodes，不再单独检出 HelpCode。发布词库只通过 product-lock.json 消费 Engine 的公开词库产物，禁止在平台复制建库/分表/压缩算法。公共词库验证器的副本必须与 Engine gitlink 完全一致。

InputController 在每次输入变更后保存值快照，界面读取不得再次维护组合状态机。
高亮候选结束组合调用 Session::finish(index)，剩余分段仍由 Engine 提交。
本地模式和候选开关是构造时已验证的产品选项；独立英文模式状态读取 Engine 快照。
按输入方案切换辅助码开关与码表。当前 RuntimePaths::legacy() 捕获原有目录布局，
完整资源包与用户数据代际切换仍需单独接入。
