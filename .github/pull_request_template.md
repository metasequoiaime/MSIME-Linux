## 改动内容

<!-- 改了什么，以及为什么需要这个改动。 -->

## 验证方式

<!-- 实际跑过的命令与结果。至少应覆盖 ctest；涉及安装或打包时补上对应的冒烟测试。 -->

- [ ] `ctest --test-dir build --output-on-failure`
- [ ] `tests/InstallSmoke.sh build`

## 检查项

- [ ] 新增行为带有测试，并已在 `CMakeLists.txt` 中通过 `add_test` 注册
- [ ] 没有引入新的编译警告（所有目标使用 `-Werror`）
- [ ] 涉及联网行为、凭据存储或本地数据落盘时，已同步更新 `PRIVACY.md`
- [ ] 涉及按键、设置项或安装步骤变化时，已同步更新 `README.md`
