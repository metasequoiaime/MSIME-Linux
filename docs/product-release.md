# Linux 产品版本组合

一个 Linux 源码版本对应的完整一方依赖组合由两部分构成：

- **源码 pin 走 gitlink。** engine、helpcode、dict 三个 submodule 的提交由 git 本身固定，在 PR diff 里可评审，`scripts/bootstrap_ci_dependencies.sh` 按 gitlink 逐个还原，全程不执行 `git submodule update --remote`。这些提交不再抄一份进锁文件——那只会给同一个 pin 造出第二处会漂移的副本，还会挡住 `engine-bump-triage.yml` 对 dependabot 引擎升级的自动合并。
- **数据 pin 走 `product-lock.json`。** 词库是 git 唯一钉不住的输入：它是 release 资产，tag 可以被上游重新指向。锁文件记录 release tag 和每个文件的 SHA256，构建按这些**已提交的摘要**校验，而不是按 release 里随数据一起发布的 `SHA256SUMS.txt`——后者和数据一样可变，把数据和校验文件一起替换掉并不能让构建通过，因为校验文件本身也在锁里。

这与 Windows 的差别是刻意的：MSIME-Windows 的 Server / 页面 / 安装器 / 辅助码都**不是**它的 submodule，只能在锁文件里显式钉住；Linux 这边它们本来就是 submodule。

## 更新词库

```sh
python3 scripts/product_lock.py refresh --dictionary-tag dict-2026.09.05
python3 scripts/product_lock.py validate
```

`refresh` 是唯一会访问上游的命令。它下载该 release 的全部资产、算出摘要、并与 release 自带的 `SHA256SUMS.txt` 交叉核对——只有这一刻信任上游校验文件是合适的，因为这正是有人专门审视这批数据的时刻，而它产出的摘要就是此后每次构建的判据。改动落在 `product-lock.json` 的 diff 里，评审后提交。

`scripts/fetch_dictionary.py` 不再接受 `--tag`：换 release 必须经过上面这条评审路径，不能在流水线里临时指定。

## 构建时会发生什么

`fetch_dictionary.py` 先把资产下载到一个暂存目录，校验锁里的摘要，再跑 SQLite 完整性与候选探针（`ni'hao`、`yyds`、`aaaa`、`xiaolian`、`haixiu`、`hello`），全部通过后才覆盖 `vendor/MetasequoiaImeDict/out/`。下载失败或数据被替换时，上一份可用的词库原样保留，不会被替换掉一半。

## 重建与追溯

发布流水线用 `product_lock.py manifest` 生成 `product-manifest.json`，内容是被发布的源码提交、三个 gitlink 的实际提交、锁定的词库 tag 与摘要，以及锁文件自身的 SHA256。它同时随包安装到 `<prefix>/share/metasequoiaime/product-manifest.json` 并作为 release 资产附加，打包步骤会比对两者一致后才继续。

本地构建默认不生成也不安装该文件：一份过期的清单比没有清单更糟。需要复现打包时显式传入：

```sh
python3 scripts/product_lock.py manifest --source-commit "$(git rev-parse HEAD)" --output product-manifest.json
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMETASEQUOIA_IME_PRODUCT_MANIFEST="$PWD/product-manifest.json"
```

签名、工具链与 runner 更新仍可能改变二进制字节；此机制保证产品源码与数据组合确定，不承诺安装包逐字节一致。

## 本地检查

```sh
python3 -m unittest discover -s tests -p 'test_product_lock.py'
python3 scripts/product_lock.py validate
python3 scripts/product_lock.py verify-dictionaries vendor/MetasequoiaImeDict/out
```
