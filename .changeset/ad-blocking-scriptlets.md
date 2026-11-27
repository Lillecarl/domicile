---
"@domicile-desktop/engine": minor
"domicile": minor
---

The engine runs the lists' scriptlets (`##+js()`) in a website's page before its own scripts, with `browser.block_resources`, and applies their procedural filters (`:has-text()`, `:upward()`, `:remove()`) with Brave's own scripts, in Standard and Strict.
