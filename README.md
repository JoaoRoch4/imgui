# Dear ImGui — JoaoRoch4 Fork

This is a personal fork of [Dear ImGui](https://github.com/ocornut/imgui) by **ocornut**, extended with:

- A fully refactored **SDL3 + Vulkan** example featuring a layered architecture
- An in-app **multi-tab terminal** with command dispatch, history, and PTY bash sessions
- A **`COPILOT` command** that pipes questions to the `gh copilot suggest` CLI
- **NotoSans + CJK + color emoji** font loading via FreeType + PlutoSVG
- An **MCP agent** that wraps a Claude Sonnet 4.6 AI assistant specialized in Dear ImGui and LLVM/Clang, usable from the VS Code AI Toolkit

---

## What's upstream

The Dear ImGui core (`imgui.cpp`, `imgui.h`, all widgets, all backends) is **unchanged from upstream**.
See [`docs/README.md`](docs/README.md) for the original project description, gallery, and integration guide.

---

## example_sdl3_vulkan — layered architecture

The SDL3 + Vulkan example has been refactored from a single flat `main.cpp` into a clean layered design:

| File | Responsibility |
| ---- | ------------- |
| `main.cpp` | Entry point, event loop, swap-chain rebuild |
| `vulkan_context.cpp/.hpp` | Vulkan instance, device, swap chain, render pass |
| `sdl_window.cpp/.hpp` | SDL3 window + surface creation, DPI helpers |
| `imgui_layer.cpp/.hpp` | ImGui context, backends, fonts, all UI windows |
| `imgui_console.cpp/.hpp` | Interactive terminal widget (log, input, command registry) |
| `imgui_debug_log_mirror.hpp` | Realtime mirror of the ImGui debug log to a file |
| `test_engine_layer.cpp/.hpp` | imgui_test_engine integration (optional) |

### Multi-tab Terminal (`ImGuiLayer` + `ImGuiConsole`)

The **Terminals** window hosts any number of independent terminal tabs, each with its own:

- scrollable, filterable log output
- command input with Tab-completion and arrow-key history
- PTY-backed bash session

Tabs can be added at runtime with the `+` button. Internally each tab owns a
`std::unique_ptr<ConsoleCommands>`, and `DrawContents(id)` isolates widget IDs
with `PushID/PopID` so multiple instances never collide.

#### Built-in commands

| Command | Description |
| ------- | ----------- |
| `HELP` | List all registered commands |
| `CLEAR` | Clear the log |
| `BASH [cmd]` | Run a shell command or start an interactive PTY bash |
| `COPILOT <question>` | Run `gh copilot suggest '<question>'` and stream output to the log |
| `DEMO` | Toggle the Dear ImGui demo window |
| `STYLE <name>` | Switch between Dark, Light, Classic styles |
| `QUIT` | Request application exit |

### Font loading

Fonts are loaded in `ImGuiLayer::Init()`:

- **NotoSans** — primary Latin/Greek/Cyrillic font (FreeType, size 18 px)
- **NotoSansSC** — CJK supplement merged into the primary atlas
- **NotoColorEmoji** — COLRv1 color emoji merged via FreeType PlutoSVG

`imconfig.h` enables `IMGUI_USE_WCHAR32`, `IMGUI_ENABLE_FREETYPE`, and
`IMGUI_ENABLE_FREETYPE_PLUTOSVG` to support the full Unicode range.

### Build

```bash
cd examples/example_sdl3_vulkan
make -j$(nproc)
./example_sdl3_vulkan
```

**Dependencies** (resolved via pkg-config): SDL3, Vulkan, FreeType2, PlutoSVG.

---

## MCP Agents (`agents/`)

### imgui-cpp-expert

An AI agent powered by **Claude Sonnet 4.6** (via GitHub Copilot's Anthropic API),
specialized in Dear ImGui C++ and LLVM/Clang.

- Extended thinking / high reasoning effort for complex multi-step problems
- Full workspace awareness of this repository's structure and compile database
- Multi-turn interactive CLI and optional FastAPI HTTP server (VS Code AI Toolkit compatible)

```bash
cd agents/imgui-cpp-expert
python -m venv venv && source venv/bin/activate
pip install -r requirements.txt
cp .env.template .env  # add ANTHROPIC_API_KEY
python agent.py --mode interactive
```

See [`agents/imgui-cpp-expert/README.md`](agents/imgui-cpp-expert/README.md) for full documentation.

### cpp-reference

MCP server exposing the cppreference.com documentation as structured tools, for
use with AI coding assistants.

---

## Branch

Active development lives on `feature/mcp-agents-and-cpp-tools`.  
Upstream tracking: `ocornut/imgui` — synced periodically.

---

## License

Dear ImGui is MIT licensed. See [LICENSE.txt](LICENSE.txt).  
Fork additions in `examples/example_sdl3_vulkan/` and `agents/` are also MIT.
