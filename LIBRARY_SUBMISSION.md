# VCV Library Submission — Issue-Text

Repo: https://github.com/VCVRack/library → "New Issue"

**Titel:**

```
Add NTRWabot
```

**Body:**

```
Please add my plugin to the VCV Library.

- Plugin slug: NTRWabot
- Source URL: https://github.com/nevertrustrobots/NTRWabot
- License: GPL-3.0-only
- Version: 2.9.6

The plugin contains one module, "Wabot": an HTTP bridge that lets an AI
assistant (Claude, via the Model Context Protocol) inspect and control the
running patch — list modules, set parameters, connect cables, analyze audio.

Transparency notes, since a module opening a network server is unusual:

- The HTTP server binds to localhost (127.0.0.1) port 7777 only — it is never
  exposed to the network.
- It only runs after the user explicitly presses the START button on the
  module panel; a lit orb indicates the server is active.
- No telemetry, no external connections of any kind; the module never
  initiates outgoing traffic. It only answers local requests.
- The companion MCP server/skill that connects Claude to the module is a
  separate (non-library) component; the module is fully self-contained and
  harmless without it.

CI builds for mac-x64, mac-arm64, win-x64 and lin-x64 run on every push:
https://github.com/nevertrustrobots/NTRWabot/actions

Thanks for reviewing!
```
