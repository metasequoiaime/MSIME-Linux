# 参与贡献

感谢参与水杉输入法 Linux 前端的开发。本仓库只负责 Linux／IBus 适配层、安装、打包与 CI；组词逻辑、词库与辅助码分别属于 [`MSIME-Engine`](https://github.com/metasequoiaime/MSIME-Engine)、[`MSIME-Dict`](https://github.com/metasequoiaime/MSIME-Dict) 与 [`MetasequoiaImeHelpCode`](https://github.com/metasequoiaime/MetasequoiaImeHelpCode)，请把对应问题提到那些仓库。

## 准备开发环境

依赖清单见 [README](README.md#依赖)。取得源码后需要拉起被固定的子模块：

```sh
git submodule update --init --recursive
```

在 git 传输不可用的环境（包括 CI）里，改用 `scripts/bootstrap_ci_dependencies.sh`，它按 gitlink 中固定的修订版本通过 GitHub 归档 API 取回同样的内容。子模块缺失时 CMake 会直接报错并提示这两条路径。

## 构建与验证

提交前请在本地跑完整门禁，与 CI 保持一致：

```sh
python3 scripts/build_dictionary.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
tests/InstallSmoke.sh build
```

所有目标都带 `-Wall -Wextra -Wpedantic -Werror`，任何新增警告都会让构建失败。`ctest` 里包含真实 D-Bus 的 `ibus_smoke`、需要 `gnome-keyring` 的 `secret_store`，以及 `packaging_tgz` 与 `packaging_deb` 打包门禁；缺少 `dpkg-deb` 的机器会自动跳过 DEB 那项。

没有 IBus 头文件时可以用 `-DMETASEQUOIA_IME_BUILD_IBUS=OFF` 只构建受测核心，但这样会跳过大部分门禁，不能替代完整验证。

## 代码风格

沿用文件里已有的风格：四空格缩进，行宽 120，C++17，Allman 风格大括号，代码注释一律用英文。不要在本仓库跑 `clang-format` 全量格式化——当前代码与 `MSIME-Engine` 的 `.clang-format` 尚未对齐，全量格式化会淹没有意义的改动。

新增行为必须带测试。测试放在 `tests/` 下，并在 `CMakeLists.txt` 里用 `add_test` 注册，让它进入 CI 门禁。

## 提交与合并

- 使用 conventional commits，带模块 scope，例如 `feat(ibus): ...`、`fix(settings): ...`、`test(voice): ...`、`chore(ci): ...`。
- 在特性分支上开发，通过 Pull Request 合入 `main`。
- PR 需要说明改了什么、为什么改，以及验证方式。CI（Ubuntu IBus）必须通过。
- 改动如果涉及联网行为、凭据存储或本地数据落盘，请同步更新 [PRIVACY.md](PRIVACY.md)。

## 发布

发布由 release-please 驱动，与 [`MSIME-Apple`](https://github.com/metasequoiaime/MSIME-Apple) 保持一致，流程如下：

1. conventional commits 合入 `main` 后，`Release` 流水线自动开出一个版本 PR，其中包含 `version.txt`、`CMakeLists.txt` 中的版本号与 `CHANGELOG.md` 的更新。
2. 合并该 PR 即打出 `vMAJOR.MINOR.PATCH` 标签并创建草稿 Release。
3. 发布流水线随后校验该提交确实在 `main` 历史中、`version.txt` 与标签一致，然后以 Release 模式构建、跑完整测试与安装冒烟、用 CPack 生成 TGZ／DEB／RPM，附加到 Release 并将其转正。

不要手动修改 `version.txt`、`.release-please-manifest.json` 或 `CMakeLists.txt` 里的 `project(... VERSION ...)`，这三处由 release-please 统一维护。需要重新发布某个已存在的草稿时，用 `Release` 工作流的手动触发并传入标签名。

## 安全问题

疑似漏洞不要走公开 issue，按 [SECURITY.md](SECURITY.md) 的方式私下上报。
