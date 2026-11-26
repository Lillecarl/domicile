---
"domicile": minor
"@domicile-desktop/sdk": minor
---

The engine blocks ads and trackers itself, with Brave's filter engine, when the config has a `content_blocking` section: filter lists by address or path, scriptlets, element hiding, and a mode per site. A shell reads and sets a site's mode with `browser.blockingFor` and `browser.setSiteBlocking`, and each browser window counts its page's `blockedRequests`.
