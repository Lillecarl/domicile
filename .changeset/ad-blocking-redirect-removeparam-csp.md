---
"@domicile-desktop/engine": minor
"domicile": minor
---

The engine acts on the lists' `$redirect`, `$removeparam` and `$csp` rules: a blocked request a `$redirect` names is answered with the stand-in from `browser.block_resources`, an allowed one loses the parameters `$removeparam` removes, a document gets the policy `$csp` adds, and a server's redirect to a blocked address fails.
