# Changelog

## [0.9.1](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.9.0...v0.9.1) (2026-09-06)


### Bug Fixes

* **ci:** separate automatic and manual release locks ([ad389c6](https://github.com/metasequoiaime/MSIME-Linux/commit/ad389c6ead0857befd83cc7eecd7ef04fc58cf4c))
* **ci:** separate automatic and manual release locks ([78fc7af](https://github.com/metasequoiaime/MSIME-Linux/commit/78fc7af451cd2c07957afd2394ff4b4881e4f50a))
* **packaging:** 修正 ibus 依赖等级，并让发行版能离线构建 ([966139d](https://github.com/metasequoiaime/MSIME-Linux/commit/966139da8574f12868fa78c505a3b1c139612fb4))

## [0.9.0](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.8.2...v0.9.0) (2026-09-06)


### Features

* **release:** separate the automatic build channel from the release channel ([13a07a5](https://github.com/metasequoiaime/MSIME-Linux/commit/13a07a55c6f7d402d43b3c024b8a24b0d0cb306d))
* **release:** separate the automatic build channel from the release channel ([0fc7b2d](https://github.com/metasequoiaime/MSIME-Linux/commit/0fc7b2d814bb9769a93f50f50b8cbb9e96449269))


### Bug Fixes

* repository-wide audit — 30 defects across input, settings, tools, online and packaging ([#86](https://github.com/metasequoiaime/MSIME-Linux/issues/86)) ([6c7a88d](https://github.com/metasequoiaime/MSIME-Linux/commit/6c7a88d841ca5ea2497e1f69dd6994c85e696663))

## [0.8.2](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.8.1...v0.8.2) (2026-09-06)


### Bug Fixes

* **ibus:** prevent stale composition from entering newly focused GTK fields ([#85](https://github.com/metasequoiaime/MSIME-Linux/issues/85)) ([af3fc18](https://github.com/metasequoiaime/MSIME-Linux/commit/af3fc18617f0470d3db07ebf5c5956bc14508e69))
* 修复在线工作线程生命周期、词库播种静默失败与工具栏语音结果丢失 ([#81](https://github.com/metasequoiaime/MSIME-Linux/issues/81)) ([f724ecd](https://github.com/metasequoiaime/MSIME-Linux/commit/f724ecd5fa3945856fbd1ce48a3b753948681a26))

## [0.8.1](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.8.0...v0.8.1) (2026-09-05)


### Bug Fixes

* **install:** resolve consolidated helpcodes in source installs and smoke tests ([ae92220](https://github.com/metasequoiaime/MSIME-Linux/commit/ae92220846416bb83452c0eb337e0fa8af968a22))

## [0.8.0](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.7.0...v0.8.0) (2026-09-05)


### Features

* **helpcode:** show helpcode hints beside dictionary candidates ([9a47616](https://github.com/metasequoiaime/MSIME-Linux/commit/9a476169cab66e93060590163d196934c5f24704))
* **helpcode:** show helpcode hints beside dictionary candidates ([2d6f63f](https://github.com/metasequoiaime/MSIME-Linux/commit/2d6f63f2c47e60099da4d74eb73bc04973c6e831))
* **input:** follow Windows punctuation lock on language switches ([baf3a65](https://github.com/metasequoiaime/MSIME-Linux/commit/baf3a65f97318976158f07726e1186b5a01fc4e6))
* **input:** follow Windows punctuation lock on language switches ([645e8bb](https://github.com/metasequoiaime/MSIME-Linux/commit/645e8bb4f307d59c726f70d75bf8b8fc6826905b))
* **input:** reset to a configured mode when the input method is activated ([20e7b59](https://github.com/metasequoiaime/MSIME-Linux/commit/20e7b59fd3bd4d5ccdeaeab02a0d8881fbf17d58))
* **input:** reset to a configured mode when the input method is activated ([9e32eac](https://github.com/metasequoiaime/MSIME-Linux/commit/9e32eac682c6c63c1e57bd99284c65f1c05bc88a))
* **keybindings:** make the Chinese/English toggle configurable ([3a80064](https://github.com/metasequoiaime/MSIME-Linux/commit/3a800645a227af140974288bebb0a883db4cc34e))
* **keybindings:** make the Chinese/English toggle configurable ([214f524](https://github.com/metasequoiaime/MSIME-Linux/commit/214f524c9872c96e7473c8911611a3519c9ef3dc))
* **utility:** let the Shift+key local modes be turned off ([b25a7e9](https://github.com/metasequoiaime/MSIME-Linux/commit/b25a7e94feb9cbf4e2df402c034ffbf07c2cc306))
* **utility:** let the Shift+key local modes be turned off ([eac81cd](https://github.com/metasequoiaime/MSIME-Linux/commit/eac81cd8cadc047923559186a800a8ed30e11d76))


### Bug Fixes

* **input:** preserve partial composition across host transitions ([af6e0a1](https://github.com/metasequoiaime/MSIME-Linux/commit/af6e0a16b2a917a62c521f17419c4ec884bbdced))

## [0.7.0](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.6.2...v0.7.0) (2026-09-05)


### Features

* **dictionary:** validate shared product formats and package provenance ([d3bc406](https://github.com/metasequoiaime/MSIME-Linux/commit/d3bc406516ed3e37f294b0c7da6e001405091f48))


### Bug Fixes

* **dictionary:** keep fetch_dictionary.py parsable on Python 3.11 ([c2aa79f](https://github.com/metasequoiaime/MSIME-Linux/commit/c2aa79faf87b79f4f71ed52df3a95865d986d715))

## [0.6.2](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.6.1...v0.6.2) (2026-09-05)


### Bug Fixes

* **install:** give a current-session recipe that keeps the desktop's IBus ([5dfcd6f](https://github.com/metasequoiaime/MSIME-Linux/commit/5dfcd6f6b6bc614c5ba7856f477d24ec0cbc1f91))

## [0.6.1](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.6.0...v0.6.1) (2026-09-05)


### Reverts

* move back off calendar versioning ([bc8173d](https://github.com/metasequoiaime/MSIME-Linux/commit/bc8173d96350ddc8003253617097e5cd4c9f6856))

## [0.6.0](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.5.0...v0.6.0) (2026-09-05)


### Features

* **product:** lock the shipped dictionary by digest and record a product manifest ([5089ca8](https://github.com/metasequoiaime/MSIME-Linux/commit/5089ca848de163ea3ca7b5ab28bf6acb2868033e))
* **product:** lock the shipped dictionary by digest and record a product manifest ([c6a8b12](https://github.com/metasequoiaime/MSIME-Linux/commit/c6a8b12579aea80e4887fcfb8b817eec7539f9e4))


### Bug Fixes

* **ci:** check out the scripts the prepare job now runs ([9771edd](https://github.com/metasequoiaime/MSIME-Linux/commit/9771edd120b8398ee41127e0d081fadb18d8c127))
* **ci:** check out the scripts the prepare job now runs ([4a429c8](https://github.com/metasequoiaime/MSIME-Linux/commit/4a429c80d4ec28c6dc9dbee6cfa5675f9d5cdf24))
* **ci:** dispatch for every release head, not just the first one ([bfdccbe](https://github.com/metasequoiaime/MSIME-Linux/commit/bfdccbe72052774d4c60025720a30e065e1de636))
* **ci:** dispatch for every release head, not just the first one ([4bea2d7](https://github.com/metasequoiaime/MSIME-Linux/commit/4bea2d78de79fdb3eb055b0fe0c10bb93c7f1ed4))
* **ci:** keep the literal token name out of ci.yml ([bf4589e](https://github.com/metasequoiaime/MSIME-Linux/commit/bf4589e1b1f2d3b3cad12b679b6f4dcd609e82d4))
* **ci:** stop the gated pull_request run cancelling the dispatched one ([947140f](https://github.com/metasequoiaime/MSIME-Linux/commit/947140f085a7e3ca99c0ae1e3eb3a7627a0e3f01))
* **ci:** stop the gated pull_request run cancelling the dispatched one ([9b1ca8a](https://github.com/metasequoiaime/MSIME-Linux/commit/9b1ca8ae7001762e6379cb6d7d41facc4cf30c16))
* **product:** ask ls-remote for the peeled ref by name ([70e683d](https://github.com/metasequoiaime/MSIME-Linux/commit/70e683d83c79e203f02eaa6970b14d4de38157e4))
* **product:** lock the dictionary's source commit instead of a gitlink ([4e3995f](https://github.com/metasequoiaime/MSIME-Linux/commit/4e3995fb150ee3e4037e79bb150a8532d26c3926))
* **product:** 词库源码提交改为锁文件记录，删掉 Dict submodule ([99a5a86](https://github.com/metasequoiaime/MSIME-Linux/commit/99a5a864dd6cb54a7a172077dbb7f41da901babf))

## [0.5.0](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.4.3...v0.5.0) (2026-09-05)


### Features

* **dictionary:** take the dictionaries from an MSIME-Dict release ([46d380a](https://github.com/metasequoiaime/MSIME-Linux/commit/46d380a3fc77a33b83ec1c43339cf42c24d14019))
* **dictionary:** take the dictionaries from an MSIME-Dict release ([ef4b15b](https://github.com/metasequoiaime/MSIME-Linux/commit/ef4b15b8b79b4d02d317c70c1cdeec6644e78ef9))
* skip clipboard content a password manager marked as secret ([b7b1c6a](https://github.com/metasequoiaime/MSIME-Linux/commit/b7b1c6afe5f0b5b598bb2092b5c7448d5621b976))
* skip clipboard content a password manager marked as secret ([acbefe3](https://github.com/metasequoiaime/MSIME-Linux/commit/acbefe3cf1a8aae6080e5246dafface2197ebeac))


### Bug Fixes

* **dictionary:** move the Dict pin off the revision with real personal data ([b69bfcf](https://github.com/metasequoiaime/MSIME-Linux/commit/b69bfcf654f7b799df522489ae0e71a2bb48d121))
* download over plain HTTPS instead of the GitHub CLI ([61f4985](https://github.com/metasequoiaime/MSIME-Linux/commit/61f49855a8fcc2f3c4c21b0c4a287657d65b9435))

## [0.4.3](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.4.2...v0.4.3) (2026-09-05)


### Bug Fixes

* **dictionary:** build the wubi86 table ([131bd44](https://github.com/metasequoiaime/MSIME-Linux/commit/131bd44e89e0b0aed686cd0e82dd277b0951cb47))

## [0.4.2](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.4.1...v0.4.2) (2026-09-05)


### Bug Fixes

* reject a malformed credential when it is saved, not when it is used ([22af212](https://github.com/metasequoiaime/MSIME-Linux/commit/22af212e17e1c4f4ca0c70f08fdb29a0aed79dfb))

## [0.4.1](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.4.0...v0.4.1) (2026-09-05)


### Bug Fixes

* apply the same credential check in every provider ([04bc6ed](https://github.com/metasequoiaime/MSIME-Linux/commit/04bc6ed0985f4724fd5f543b4c809f9d6c527546))
* apply the same credential check in every provider ([07c269e](https://github.com/metasequoiaime/MSIME-Linux/commit/07c269e3541c95e63e1b6539f3d93f0d60d03318))
* decide once which endpoints may receive a credential ([c50da31](https://github.com/metasequoiaime/MSIME-Linux/commit/c50da31bfcec97095d8fd7a3006829d8dced57e5))
* decide once which endpoints may receive a credential ([272253b](https://github.com/metasequoiaime/MSIME-Linux/commit/272253b022723356a9c87966a42372e2df10aab8))

## [0.4.0](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.3.0...v0.4.0) (2026-09-05)


### Features

* say why there are no candidates when the dictionary is unusable ([083ca3e](https://github.com/metasequoiaime/MSIME-Linux/commit/083ca3e5d3cbebeb6403af16bfa5ae07e718e86d))
* say why there are no candidates when the dictionary is unusable ([91fca47](https://github.com/metasequoiaime/MSIME-Linux/commit/91fca470e6ff92387d4d99da5260b7300096f8e8))


### Bug Fixes

* resolve the desktop tools next to the running program everywhere ([8ce9bfd](https://github.com/metasequoiaime/MSIME-Linux/commit/8ce9bfd55bcabbc9553c5146e674b9f5a743d585))
* resolve the desktop tools next to the running program everywhere ([b0891e0](https://github.com/metasequoiaime/MSIME-Linux/commit/b0891e0d1834fe31e5cb440dace9c50b39e23bcc))

## [0.3.0](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.2.3...v0.3.0) (2026-09-05)


### Features

* seed the user data directory from the packaged dictionaries ([c7b5085](https://github.com/metasequoiaime/MSIME-Linux/commit/c7b508598f9551190b380bdb5f8a49c2d427dd6e))

## [0.2.3](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.2.2...v0.2.3) (2026-09-05)


### Bug Fixes

* make the published packages actually usable ([980b36c](https://github.com/metasequoiaime/MSIME-Linux/commit/980b36c5baf42db52e837a6ed2ff78a758b7a4c0))
* make the published packages actually usable ([589f397](https://github.com/metasequoiaime/MSIME-Linux/commit/589f397b046d3d4b2f130ccce7db0e4a57799cea))

## [0.2.2](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.2.1...v0.2.2) (2026-09-05)


### Bug Fixes

* give the release archive an architecture-specific name ([ad09e4f](https://github.com/metasequoiaime/MSIME-Linux/commit/ad09e4fba61ce26d28054a36a7ff1b0367f11de6))
* give the release archive an architecture-specific name ([be1a987](https://github.com/metasequoiaime/MSIME-Linux/commit/be1a987f0975150a763ee99edd65b85e617ef062))

## [0.2.1](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.2.0...v0.2.1) (2026-09-05)


### Bug Fixes

* make the current-user install work on Ubuntu 26.04 ([57028ca](https://github.com/metasequoiaime/MSIME-Linux/commit/57028cadbcfaa9662509acec1d79408c0959c2f3))
* make the current-user install work on Ubuntu 26.04 ([c3b4f24](https://github.com/metasequoiaime/MSIME-Linux/commit/c3b4f2439992f51b6c4abe4c2280b02367515f1b))

## [0.2.0](https://github.com/metasequoiaime/MSIME-Linux/compare/v0.1.0...v0.2.0) (2026-09-05)


### Features

* add Linux IBus frontend ([4833d88](https://github.com/metasequoiaime/MSIME-Linux/commit/4833d88f1d7f93bb51c7ccfdbc677c89ba2af720))
* **desktop:** add handwriting recognition and floating toolbar ([9f32c41](https://github.com/metasequoiaime/MSIME-Linux/commit/9f32c41629982267fdb069f707dedfa6e7a497ca))
* **ibus:** add desktop input experience and local modes ([#1](https://github.com/metasequoiaime/MSIME-Linux/issues/1)) ([58f1301](https://github.com/metasequoiaime/MSIME-Linux/commit/58f13014ad19192bcf985c67cb1b0da1724d7927))
* **ibus:** publish asynchronous online candidates ([c0e22e0](https://github.com/metasequoiaime/MSIME-Linux/commit/c0e22e01909c3bf71d2a98bf0383b4abc40d2e92))
* **ibus:** show candidate translations ([12b7585](https://github.com/metasequoiaime/MSIME-Linux/commit/12b758569b5f1afdddac504247b8552670c32d89))
* **ibus:** track online composition generations ([2b733a0](https://github.com/metasequoiaime/MSIME-Linux/commit/2b733a05b0e422123f52682fc85a9b9a2ebba210))
* **linux:** align Windows capabilities and settings UI ([325631a](https://github.com/metasequoiaime/MSIME-Linux/commit/325631af356a834aeb65693cd73b9bd6c7367a04))
* **online:** add cancellable Google cloud candidates ([779f7c6](https://github.com/metasequoiaime/MSIME-Linux/commit/779f7c62d263f11c9233cb4b5fc698675bb00395))
* **online:** add OpenAI-compatible AI suggestions ([ee81b52](https://github.com/metasequoiaime/MSIME-Linux/commit/ee81b52b5a5708042d320f1c537584760f3f649f))
* **settings:** align Linux UI with Windows layout ([8081ede](https://github.com/metasequoiaime/MSIME-Linux/commit/8081ede1eac1574c7514e6d1745dbf6ec309e7bf))
* **settings:** configure online providers securely ([dfd35a4](https://github.com/metasequoiaime/MSIME-Linux/commit/dfd35a45d4878c79db38d1ba8625195e2c0a4b21))
* **voice:** add Linux microphone capture CLI ([551a61c](https://github.com/metasequoiaime/MSIME-Linux/commit/551a61cf50cc0705bc317c0e53cdd3c6fa74a86b))
* **voice:** add optional transcript polishing ([f2cd568](https://github.com/metasequoiaime/MSIME-Linux/commit/f2cd568161c948ca30adc3a17f46a5171d192c22))


### Bug Fixes

* **ci:** build against Ubuntu system packages ([9622684](https://github.com/metasequoiaime/MSIME-Linux/commit/96226844298d03a9eaa3894d288c9f49a6e0bbf5))
* **settings:** preserve rows while applying editors ([b1d52e3](https://github.com/metasequoiaime/MSIME-Linux/commit/b1d52e337ecc80ce143f0cbdcafe8adb6e4e271b))

## Changelog

Entries below this line are generated by release-please from conventional commit messages. Do not edit them by hand.
