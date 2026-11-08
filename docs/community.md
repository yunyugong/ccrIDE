# GitHub community hosting

ccrIDE 0.3.7.2 can use GitHub for source, issues, documentation, plugin catalogs and package downloads. It does not require a custom web server. Network access starts when the user refreshes the marketplace or installs a plugin; startup does not automatically check for updates or download code.

## Local link configuration

The offline package includes `data/community.json`. Edit it and restart:

```json
{
  "schema": 1,
  "repository": "https://github.com/yunyugong/ccrIDE",
  "releases": "https://github.com/yunyugong/ccrIDE/releases",
  "issues": "https://github.com/yunyugong/ccrIDE/issues",
  "documentation": "https://github.com/yunyugong/ccrIDE/blob/main/docs/plugin-api.md",
  "marketplace": "https://raw.githubusercontent.com/yunyugong/ccrIDE/main/community/marketplace.json"
}
```

The schema is 1, file limit 64 KiB. Link values must be HTTPS URLs or empty strings. Invalid files produce a console message and retain built-in defaults. An empty link disables its Help-menu action. Help links open in the system browser. This is a local configuration file, not a remotely executed configuration.

`marketplace` supplies the initial URL when no saved marketplace setting overrides it. A saved `marketUrl` in `data/settings.json` wins, including an empty value (offline mode). In Settings > Plugins, **Use Community Marketplace URL** copies the configured default; click **Save Settings** to persist it. The existing custom URL field remains available.

## Catalog format

Host the catalog as a UTF-8 JSON file in a public repository. Use its **Raw** URL rather than the GitHub HTML `blob` page. New catalogs use a versioned envelope:

```json
{
  "schema": 1,
  "plugins": [
    {
      "id": "community-example",
      "name": "Community Example",
      "version": "1.0.0",
      "author": "Your name",
      "description": "Example plugin for ccrIDE",
      "entry": "example.dll",
      "apiVersion": 2,
      "packageUrl": "https://github.com/OWNER/REPOSITORY/releases/download/v1.0.0/community-example-v1.0.0.zip",
      "sha256": "REPLACE_WITH_THE_64_HEX_DIGIT_SHA256_OF_THE_COMPLETE_ZIP"
    }
  ]
}
```

The example digest above is deliberately a placeholder: replace it with the actual digest before publishing. `community/marketplace.json` lists currently published packages with real digests. The v0.3.7.2 candidate catalog is `community/marketplace-v0.3.7.2.json`; promote it only after its Release assets are uploaded. This avoids advertising unavailable downloads.

- Catalog limit: 2 MiB and 500 entries. Malformed/incompatible entries reject the fetched catalog; the installed plugins remain unchanged.
- Required: valid unique `id`, nonempty `version`, safe `.dll` entry, supported `apiVersion`, exactly one download URL, and a 64-digit SHA-256 digest. Names/descriptions should be understandable internationally; English is recommended as the baseline for third-party catalog metadata.
- `packageUrl` downloads a complete plugin ZIP, including service programs and clangd if bundled. The ZIP must contain one top-level plugin directory. Its manifest ID, version, DLL entry, API version and `taskCoordinatorVersion` must match the catalog. The extracted DLL is checked as Windows x64 before committing the folder.
- Legacy root-array catalogs and `dllUrl` single-DLL downloads remain readable, but now require a digest and a secure URL as well. Never specify both URL fields.
- Public download URLs require HTTPS. For local development tests only, HTTP to `127.0.0.1:<port>` or `localhost:<port>` is allowed.
- Downloads are limited to 64 MiB; extracted plugin ZIP content is limited to 128 MiB. Failed/truncated/oversized downloads, bad digests or mismatched manifests do not commit a plugin directory. The worker cleans its staging files.
- Existing plugin directories are not overwritten. Uninstall the previous version first. Click Reload Plugins after an online installation.

A download can use GitHub's tagged Release asset URL, as above. Use a version-specific URL so the catalog version and checksum identify one immutable package. Keep the catalog on `main` if you want maintainers to update it through pull requests. Users on networks where GitHub is unavailable may download ZIPs elsewhere and install locally; the IDE does not silently switch mirrors.

## Release maintainer workflow

1. Build and test the IDE/plugins. Preserve old version artifacts.
2. Package each plugin with `packaging/package-plugins.py --output <new-output-directory>`; this writes matching `.sha256` files.
3. Run `tools/generate-marketplace.py --artifacts <new-output-directory> --tag v0.3.7.2 --output community/marketplace-v0.3.7.2.json`. It reads manifest metadata and computes actual ZIP digests; it does not upload anything.
4. Upload the listed plugin ZIPs and checksum files to that GitHub Release.
5. Replace `community/marketplace.json` with the candidate catalog and commit it after its asset URLs are available. Verify a fresh download through the marketplace before announcing availability.
6. Publish bilingual release notes, with English first, and attach the Windows x64 installer and portable bundle.

The client does not create GitHub releases, handle private-repository authentication, auto-update itself, or treat a catalog listing as proof of plugin safety. Hosted files remain subject to GitHub availability and repository visibility.

## Translation contributions

- Editable English source: `locales/en.json` (`format=ccrIDE.translations`, `schema=1`). Chinese UI source strings are the keys.
- Keep printf placeholders, NUL-separated combo entries, URLs and stable IDs intact. Do not translate user source code, extracted header comments, compiler messages or user paths.
- Run `python tools/generate-locales.py`, then `python tools/generate-locales.py --check`; commit both JSON and `src/i18n_catalog.inc`. Translations are compiled into the binary, so changing JSON alone in an installed folder does not alter the interface.
- Rebuild and inspect both languages, dark/light themes, narrow windows and large interface font sizes. The current runtime choices are System Default, Simplified Chinese and English; additional languages need code/catalog integration, not merely a new JSON filename.

中文快速说明：社区地址保存在安装目录的 `data/community.json`；设置中的 `marketUrl` 优先，留空可离线使用。插件清单放 GitHub Raw，ZIP 放 Releases，必须提供真实 SHA-256。上传插件附件后再提交对应清单。接口详见 [Plugin API](plugin-api.md)，翻译由 `locales/en.json` 生成并随构建发布。

References: [GitHub release links](https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases), [GitHub Releases API](https://docs.github.com/en/rest/releases/releases).
