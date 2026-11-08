# Contributing to ccrIDE

Welcome! English and Chinese issues and pull requests are both accepted.

- For bugs, include the IDE version, Windows/compiler versions, UI language, reproduction steps and a minimal example. Remove credentials and private data from logs/configuration files.
- Discuss substantial changes in an issue before changing the plugin ABI, source model or task coordinator. Keep existing docking, shortcuts, settings and plugin compatibility where possible.
- Build using [the project instructions](docs/building.en.md). Run the relevant tests and the catalog validator. For UI changes, check both languages, narrow windows and large fonts.
- Translate `locales/en.json`, run `tools/generate-locales.py`, and commit the generated catalog. See [translation rules](docs/community.md#translation-contributions).
- Plugins should use the [documented API](docs/plugin-api.md), keep UI callbacks short and run expensive work asynchronously. Include license information for bundled dependencies.
- Community catalogs must identify real release packages with matching hashes. Do not add private keys, local absolute paths, temporary logs, development sessions or generated toolchain binaries to source commits.

中文：欢迎中文或英文反馈。请提供版本、复现步骤和最小示例，清理密钥及私人数据。翻译修改 JSON 后须重新生成并验证；较大的架构或接口变更请先讨论，保留旧设置和插件兼容。
