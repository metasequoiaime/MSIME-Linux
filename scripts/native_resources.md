`scripts/native_resources.py build-lock` 从产品锁与 Engine 公共资产清单生成资源
锁，包含全部辅助码文件摘要；`prepare` 接收可信资源锁，把文件冻结到私有
临时目录后调用现有公共摘要/产品来源校验器，再以 Linux renameat2 的
RENAME_NOREPLACE 原子发布到内容摘要命名的目录。缓存必须重新通过完整校验
才能复用，不覆盖或修复损坏缓存；额外文件、符号链接和取消都会被拒绝或中止。
该工具只准备资源，不启用词库，不更改用户日志。发布包通过 CMake 生成并携带可信资源锁，工具与公共校验模块安装在
libexec/metasequoia-native-resources；DEB/RPM 声明 Python 3.10 以上依赖。
个人安装在回放用户日志之前保留一份已验证的基础资源，卸载会移除工具，
普通卸载保留用户数据目录中的资源与代际，--purge 按既有目录保护规则处理。
恢复界面调用该工具的步骤仍需接入。
