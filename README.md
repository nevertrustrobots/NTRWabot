# Nevertrustrobots Wabot

VCV Rack module that opens an HTTP bridge on **port 7777**, enabling Claude AI
to read and control your patch in real time — add modules, connect cables,
read voltages, set parameters, save patches, and more.

---

## What it does

When you press **START** on the Wabot module, it launches a local HTTP server.
Claude AI connects to this server through an MCP (Model Context Protocol) bridge
and gains full live access to your patch.

**The module alone does nothing useful.** You need the MCP Server + Skill
to connect it to Claude.

---

## Installation (VCV Rack Library)

1. Open the VCV Rack Library in VCV Rack
2. Search for **Nevertrustrobots** or **Wabot**
3. Install — done

Or download the `.vcvplugin` for your platform from the
[Releases page](https://github.com/nevertrustrobots/NTRWabot/releases)
and place it in your `Rack2/plugins/` folder.

---

## MCP Server + Claude Skill

To use Wabot with Claude you need:

| Component | Description |
|---|---|
| **NTR_Wabot_MCP.py** | Python MCP server that bridges Claude Desktop to the module |
| **NTR_Wabot_skill** | Claude skill with patch intelligence, signal topology reasoning, and workflow templates |

Both are available exclusively at:

**[patreon.com/c/nevertrustrobots](https://www.patreon.com/c/nevertrustrobots)**

---

## Capabilities (with MCP + Skill)

- List all modules, parameters, cables and signal levels
- Add, move, delete modules
- Connect and disconnect cables by port name or index
- Read live voltages and polyphony per output
- Set any parameter in native range
- Bypass modules for A/B comparison
- Save/restore module presets
- Save the patch
- Find unpatched ports
- Search the module library

---

## Building from source

Requires the [VCV Rack SDK](https://vcvrack.com/downloads).

```bash
# Set RACK_DIR to your Rack SDK path
RACK_DIR=/path/to/Rack-SDK make
```

Cross-platform CI builds run automatically via GitHub Actions on every push
and produce `.vcvplugin` packages for all four platforms on version tags.

---

## License

The VCV Rack module source code is licensed under **GPL-3.0-only**.
See [LICENSE](LICENSE) for details.

The MCP Server and Claude Skill are proprietary —
available at [patreon.com/c/nevertrustrobots](https://www.patreon.com/c/nevertrustrobots).
