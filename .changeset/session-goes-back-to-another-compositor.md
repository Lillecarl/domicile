---
"domicile": patch
---

A desktop on a second tty gives the session back to another compositor that still runs on the first, such as niri, when it exits. It sets that compositor's variables in the user manager and on the session bus, restarts the portal frontend under its name, and stops only `domicile-session.target`. A desktop that never comes up gives back the variables it replaced.
