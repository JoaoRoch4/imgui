#!/usr/bin/env python3
"""
ImGui C++ Expert — MCP Server
Exposes the agent as an MCP tool that VS Code Copilot Chat can invoke.
Run: python mcp_server.py
VS Code will call it as a subprocess via .vscode/mcp.json
"""

import asyncio
import glob as _glob
import html as _html_mod
import os as _os
import re as _re
import subprocess as _subprocess
import sys
import time as _time
from urllib.error import HTTPError as _HTTPError
from urllib.request import Request as _UrlReq, urlopen as _urlopen

sys.path.insert(0, __file__.rsplit("/", 1)[0])  # add agent dir to path

from mcp.server.fastmcp import FastMCP
from agent import create_client, chat_with_agent, SYSTEM_PROMPT, MODEL

mcp = FastMCP(
    name="imgui-cpp-expert",
    instructions=SYSTEM_PROMPT,
)

# One shared client (token fetched once at startup)
_client = None
_sessions: dict[str, list[dict]] = {}


def _get_client():
    global _client
    if _client is None:
        _client = create_client()
    return _client


@mcp.tool(description="Ask the ImGui C++ Expert a question. Maintains session context across calls.")
async def ask_imgui_expert(
    question: str,
    session_id: str = "default",
) -> str:
    """
    Ask the ImGui / LLVM+Clang expert agent a question.

    Args:
        question:   The C++/ImGui/LLVM question to ask
        session_id: Optional session name to keep conversation context (default: 'default')

    Returns:
        The expert's response as plain text (may include Markdown)
    """
    client = _get_client()
    if session_id not in _sessions:
        _sessions[session_id] = []

    response, updated = await chat_with_agent(question, _sessions[session_id], client)
    _sessions[session_id] = updated
    return response


@mcp.tool(description="Reset conversation history for a session (start fresh).")
def reset_session(session_id: str = "default") -> str:
    """Clear conversation history for the given session."""
    _sessions.pop(session_id, None)
    return f"Session '{session_id}' reset."


@mcp.tool(description="List active conversation sessions.")
def list_sessions() -> list[str]:
    """Return names of all active (non-empty) sessions."""
    return list(_sessions.keys())


# ── clang-tidy documentation helpers ─────────────────────────────────────────

_CT_BASE     = "https://clang.llvm.org/extra/clang-tidy/checks"
_CT_LIST_URL = f"{_CT_BASE}/list.html"

# All known clang-tidy check categories (sorted longest-first for prefix matching)
_CT_CATEGORIES: list[str] = sorted([
    "abseil", "altera", "android", "boost", "bugprone", "cert",
    "clang-analyzer", "concurrency", "cppcoreguidelines", "darwin",
    "fuchsia", "google", "hicpp", "linuxkernel", "llvm", "llvmlibc",
    "misc", "modernize", "mpi", "objc", "openmp", "performance",
    "portability", "readability", "zircon",
], key=len, reverse=True)

_CT_CACHE: dict[str, tuple[float, object]] = {}
_CT_TTL = 30 * 60.0   # 30-minute TTL


def _ct_cache_get(key: str):
    entry = _CT_CACHE.get(key)
    if not entry:
        return None
    ts, val = entry
    if _time.time() - ts > _CT_TTL:
        del _CT_CACHE[key]
        return None
    return val


def _ct_cache_set(key: str, val) -> None:
    _CT_CACHE[key] = (_time.time(), val)


async def _ct_fetch(url: str) -> tuple[bool, str]:
    """Fetch *url* in a thread (non-blocking).  Returns (ok, text_or_error)."""
    def _sync() -> tuple[bool, str]:
        try:
            req = _UrlReq(url, headers={"User-Agent": "Mozilla/5.0 (X11; Linux x86_64)"})
            with _urlopen(req, timeout=12) as resp:
                return True, resp.read().decode("utf-8", errors="replace")
        except _HTTPError as exc:
            return False, f"HTTP {exc.code}: {url}"
        except Exception as exc:
            return False, f"Error fetching {url}: {exc}"
    return await asyncio.to_thread(_sync)


def _strip_ct_html(raw: str, max_chars: int = 8000) -> str:
    """
    Strip Sphinx/RST-generated HTML from a clang-tidy doc page.

    Strategy:
      1. Remove <script>, <style>, <nav>, <footer>, <header> blocks outright.
      2. Prefer content inside the Sphinx ``.body`` / ``[role=main]`` div.
      3. Strip remaining tags, decode HTML entities, collapse whitespace.
    """
    # Remove noise blocks entirely
    raw = _re.sub(
        r"<(script|style|nav|footer|header)[^>]*>.*?</\1>",
        "", raw, flags=_re.DOTALL | _re.IGNORECASE,
    )
    raw = _re.sub(r"<!--.*?-->", "", raw, flags=_re.DOTALL)

    # Try to isolate the main content area (Sphinx layout)
    for pat in (
        r'<div[^>]+class="[^"]*\bbody\b[^"]*"[^>]*>(.*)',
        r'<div[^>]+role=["\']main["\'][^>]*>(.*)',
        r'<article[^>]*>(.*)',
        r'<section[^>]+>(.*)',
    ):
        m = _re.search(pat, raw, _re.DOTALL | _re.IGNORECASE)
        if m:
            raw = m.group(1)
            break

    # Strip all remaining tags
    raw = _re.sub(r"<[^>]+>", " ", raw)
    raw = _html_mod.unescape(raw)
    raw = _re.sub(r"[ \t]+", " ", raw)
    raw = _re.sub(r"\n{3,}", "\n\n", raw)
    return raw.strip()[:max_chars]


def _check_name_to_url(name: str) -> str | None:
    """
    Resolve a clang-tidy check name to its documentation URL.

    Accepts both dash-separated (``bugprone-use-after-move``) and
    slash-separated (``bugprone/use-after-move``) forms.
    Returns ``None`` when the category cannot be determined.
    """
    name = name.strip()
    if "/" in name:
        cat, chk = name.split("/", 1)
    else:
        cat, chk = "", name
        for c in _CT_CATEGORIES:          # longest-prefix first
            if name.startswith(c + "-"):
                cat = c
                chk = name[len(c) + 1:]
                break
        if not cat:
            return None
    return f"{_CT_BASE}/{cat}/{chk}.html"


async def _fetch_ct_check_list() -> list[str]:
    """Return the sorted list of all clang-tidy check names (cached)."""
    cached = _ct_cache_get("__list__")
    if cached is not None:
        return cached  # type: ignore[return-value]

    ok, text = await _ct_fetch(_CT_LIST_URL)
    if not ok:
        return []

    # Extract hrefs like "bugprone/use-after-move.html" or
    # "checks/bugprone/use-after-move.html" (relative or absolute fragment)
    hrefs = _re.findall(
        r'href=["\'](?:[^"\']*?/)?([\w-]+/[\w][\w-]*)\.html["\']', text
    )
    names: list[str] = sorted(set(
        f"{cat}-{chk}"
        for href in hrefs
        for cat, sep, chk in [href.partition("/")]
        if sep and chk
    ))
    _ct_cache_set("__list__", names)
    return names


# ── MCP tools: clang-tidy ─────────────────────────────────────────────────────

@mcp.tool(
    description=(
        "Search the clang-tidy check catalogue by name, category, or keyword. "
        "Returns matching check names with their documentation URLs. "
        "Examples: search_clang_tidy('modernize') → all modernize-* checks; "
        "search_clang_tidy('use-after') → bugprone-use-after-move; "
        "search_clang_tidy('nullptr') → checks related to nullptr usage."
    )
)
async def search_clang_tidy(query: str, limit: int = 15) -> str:
    """
    Search the online clang-tidy check list for checks matching *query*.

    Args:
        query: Keyword, category name, or partial check name
               (e.g. 'bugprone', 'use-after', 'nullptr', 'cppcoreguidelines')
        limit: Maximum number of results (default 15, max 50)

    Returns:
        Formatted list of matching check names with their documentation URLs.
    """
    limit = max(1, min(limit, 50))
    all_checks = await _fetch_ct_check_list()
    q = query.lower().strip()
    matches = [c for c in all_checks if q in c.lower()][:limit]

    if not matches:
        cats = ", ".join(_CT_CATEGORIES[:10])
        return (
            f"No clang-tidy checks found matching '{query}'.\n"
            f"Available categories: {cats}, …\n"
            f"Try: search_clang_tidy('bugprone') or search_clang_tidy('modernize')"
        )

    lines = [f"clang-tidy checks matching '{query}'  ({len(matches)} result(s))\n"]
    for name in matches:
        url = _check_name_to_url(name) or "—"
        lines.append(f"  {name}")
        lines.append(f"    {url}")
    return "\n".join(lines)


@mcp.tool(
    description=(
        "Fetch full documentation for a specific clang-tidy check from clang.llvm.org. "
        "Returns the check's description, rationale, configurable options, "
        "and code examples (when available). "
        "Accepts dash-separated names: 'bugprone-use-after-move', "
        "'modernize-use-nullptr', 'cppcoreguidelines-pro-bounds-pointer-arithmetic', "
        "'readability-identifier-naming', 'performance-avoid-endl'."
    )
)
async def lookup_clang_tidy(check_name: str) -> str:
    """
    Fetch and return full documentation for a clang-tidy check.

    Args:
        check_name: Dash-separated check name, e.g. 'bugprone-use-after-move',
                    'modernize-use-nullptr', 'misc-unused-parameters'.
                    Slash form also accepted: 'bugprone/use-after-move'.

    Returns:
        Full check documentation including description, options, examples,
        and the canonical documentation URL.
    """
    cache_key = f"doc::{check_name}"
    cached = _ct_cache_get(cache_key)
    if cached:
        return cached  # type: ignore[return-value]

    url = _check_name_to_url(check_name)
    if not url:
        cats = ", ".join(_CT_CATEGORIES)
        return (
            f"Cannot resolve check name '{check_name}' to a documentation URL.\n"
            f"Known categories: {cats}.\n"
            f"Example usage: lookup_clang_tidy('bugprone-use-after-move')"
        )

    ok, raw = await _ct_fetch(url)
    if not ok:
        return f"Failed to fetch documentation for '{check_name}':\n{raw}"

    text = _strip_ct_html(raw)
    result = f"# {check_name}\nURL: {url}\n\n{text}"
    _ct_cache_set(cache_key, result)
    return result


@mcp.tool(
    description=(
        "Interpret a clang-tidy diagnostic message: identify the check that triggered "
        "it, explain what it means in plain language, state why it matters, and provide "
        "remediation advice — all backed by the official clang.llvm.org documentation. "
        "Accepts raw diagnostic lines such as:\n"
        "  'warning: use std::make_unique instead [modernize-make-unique]'\n"
        "  '/path/file.cpp:42:5: warning: pointer arithmetic [cppcoreguidelines-pro-bounds-pointer-arithmetic]'\n"
        "  'error: function is not exception safe [bugprone-exception-escape]'"
    )
)
async def interpret_clang_tidy(diagnostic: str) -> str:
    """
    Parse a clang-tidy diagnostic line and return a detailed explanation.

    Args:
        diagnostic: Raw clang-tidy warning/error string, e.g.
                    'warning: use std::make_unique instead [modernize-make-unique]'

    Returns:
        Structured interpretation with:
          - Check name and documentation URL
          - Plain-language explanation of the diagnostic
          - Why the pattern is problematic
          - How to fix it (remediation steps)
          - Relevant excerpt from official documentation
    """
    # Extract [check-name] at the end of the diagnostic line
    m = _re.search(r"\[([a-z][a-z0-9]*(?:-[a-z0-9]+)+)\]\s*$", diagnostic.strip())
    if not m:
        return (
            "No clang-tidy check identifier found in the diagnostic.\n"
            "Expected format ends with '[check-name]', e.g.:\n"
            "  warning: use std::move [performance-move-const-arg]\n"
            f"Input was: {diagnostic!r}"
        )

    check_name = m.group(1)
    url = _check_name_to_url(check_name)

    doc_text = ""
    if url:
        ok, raw = await _ct_fetch(url)
        if ok:
            doc_text = _strip_ct_html(raw, max_chars=4000)

    # Strip optional 'file:line:col:' prefix from the diagnostic for display
    diag_clean = _re.sub(r"^[^:]+:\d+:\d+:\s*", "", diagnostic.strip())

    lines = [
        "## Clang-Tidy Diagnostic Interpretation",
        "",
        f"**Check**      : `{check_name}`",
        f"**Diagnostic** : {diag_clean}",
        f"**Docs URL**   : {url or '(category not resolved)'}",
        "",
    ]

    if doc_text:
        lines += [
            "### Official Documentation",
            doc_text,
        ]
    else:
        lines += [
            "### Documentation",
            "(Could not fetch — check internet connectivity or verify the check name.)",
        ]

    return "\n".join(lines)



# ── Debugger Runtime Listener ─────────────────────────────────────────────────

_CPP_TARGET = "example_sdl3_vulkan"
_LLDB_BIN   = "/usr/bin/lldb"
_CPP_EXE    = "/home/joao/vscode/imgui-1/examples/example_sdl3_vulkan/example_sdl3_vulkan"


def _find_cpp_pids() -> list[int]:
    """Return PIDs of running example_sdl3_vulkan processes."""
    try:
        r = _subprocess.run(["pgrep", "-f", _CPP_TARGET],
                            capture_output=True, text=True, timeout=3)
        return [int(p) for p in r.stdout.split() if p.strip()]
    except Exception:
        return []


def _read_proc(pid: int, name: str) -> str:
    try:
        with open(f"/proc/{pid}/{name}") as f:
            return f.read()
    except OSError:
        return ""


def _parse_proc_status(pid: int) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in _read_proc(pid, "status").splitlines():
        if ":" in line:
            k, _, v = line.partition(":")
            out[k.strip()] = v.strip()
    return out


def _thread_info(pid: int) -> list[dict[str, str]]:
    task_dir = f"/proc/{pid}/task"
    try:
        tids = sorted(int(t) for t in _os.listdir(task_dir))
    except OSError:
        return []
    threads: list[dict[str, str]] = []
    for tid in tids:
        info: dict[str, str] = {"tid": str(tid)}
        try:
            with open(f"{task_dir}/{tid}/comm") as f:
                info["name"] = f.read().strip()
        except OSError:
            info["name"] = "?"
        try:
            with open(f"{task_dir}/{tid}/status") as f:
                for line in f:
                    if line.startswith("State:"):
                        info["state"] = line.split(":", 1)[1].strip()
                        break
        except OSError:
            info["state"] = "?"
        try:
            with open(f"{task_dir}/{tid}/wchan") as f:
                info["wchan"] = f.read().strip()
        except OSError:
            info["wchan"] = "?"
        threads.append(info)
    return threads


@mcp.tool(
    description=(
        "Inspect the live state of the running example_sdl3_vulkan C++ process "
        "without interfering with any attached debugger (reads /proc only). "
        "Returns PID, thread count, per-thread state and blocked syscall (wchan), "
        "and memory usage. Works at any time — no need to pause the debugger."
    )
)
def get_cpp_process_state() -> str:
    """
    Read the live state of the running C++ process from /proc.
    Non-intrusive: safe to call while lldb-dap or gdb is attached.
    """
    pids = _find_cpp_pids()
    if not pids:
        return f"No '{_CPP_TARGET}' process found. Start the debug session first."
    lines: list[str] = []
    for pid in pids:
        status = _parse_proc_status(pid)
        threads = _thread_info(pid)
        cmdline = _read_proc(pid, "cmdline").replace("\x00", " ").strip()
        lines += [
            f"## Process PID {pid}",
            f"  State   : {status.get('State', '?')}",
            f"  Threads : {status.get('Threads', '?')}",
            f"  VmRSS   : {status.get('VmRSS', '?')}",
            f"  VmPeak  : {status.get('VmPeak', '?')}",
            f"  Cmdline : {cmdline[:120]}",
            "",
            f"  Threads ({len(threads)}):",
        ]
        for t in threads:
            lines.append(
                f"    TID {t['tid']:>6}  state={t['state']:<26}  wchan={t['wchan']:<28}  {t['name']}"
            )
        lines.append("")
    return "\n".join(lines)


@mcp.tool(
    description=(
        "Get full thread backtraces of the C++ process using lldb in batch mode. "
        "WARNING: CONFLICTS with an active lldb-dap session. "
        "Only call when the app is running WITHOUT a debugger attached, "
        "or after the debug session has stopped. "
        "For live debugger sessions use get_cpp_process_state() instead."
    )
)
async def get_cpp_thread_backtraces(pid: int = 0) -> str:
    """
    Attach lldb in batch mode to dump all thread backtraces.
    Only safe when no other debugger is already attached.
    Args:
        pid: PID to inspect (0 = auto-detect example_sdl3_vulkan)
    """
    if pid == 0:
        pids = _find_cpp_pids()
        if not pids:
            return f"No '{_CPP_TARGET}' process found."
        pid = pids[0]

    def _run() -> str:
        cmd = [_LLDB_BIN, "--batch", "-p", str(pid),
               "-o", "thread backtrace all", "-o", "quit"]
        try:
            r = _subprocess.run(cmd, capture_output=True, text=True, timeout=15)
            return (r.stdout + r.stderr).strip() or "(no output)"
        except _subprocess.TimeoutExpired:
            return "lldb timed out (15s) — another debugger may already be attached"
        except Exception as e:
            return f"Error: {e}"

    return await asyncio.to_thread(_run)


@mcp.tool(
    description=(
        "Analyze a C++ core dump file with lldb and return all thread backtraces, "
        "crashed thread info, and signal details. "
        "Leave core_path empty to auto-detect the most recent core file "
        "in the project directory and /tmp/."
    )
)
async def analyze_core_dump(core_path: str = "") -> str:
    """
    Load a core dump in lldb and extract full debug information.
    Args:
        core_path: Path to core dump. Leave empty to auto-detect the latest core.
    """
    if not core_path:
        candidates: list[str] = []
        for d in [
            "/home/joao/vscode/imgui-1/examples/example_sdl3_vulkan/",
            "/home/joao/vscode/imgui-1/",
            "/tmp/",
            _os.path.expanduser("~/"),
        ]:
            for pat in ["core", "core.*", "*.core", f"{_CPP_TARGET}.core*"]:
                candidates += _glob.glob(_os.path.join(d, pat))
        if not candidates:
            return (
                "No core dump found.\n"
                "Enable core dumps: `ulimit -c unlimited` then re-run.\n"
                "Or pass a path: analyze_core_dump('/path/to/core')"
            )
        core_path = max(candidates, key=_os.path.getmtime)
    if not _os.path.exists(core_path):
        return f"File not found: {core_path}"

    def _run() -> str:
        cmd = [_LLDB_BIN]
        if _os.path.exists(_CPP_EXE):
            cmd.append(_CPP_EXE)
        cmd += ["--core", core_path, "--batch",
                "-o", "thread list",
                "-o", "thread backtrace all",
                "-o", "quit"]
        try:
            r = _subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            out = (r.stdout + r.stderr).strip()
            return f"Core: {core_path}\n\n{out}" if out else f"Core: {core_path}\n(no output)"
        except _subprocess.TimeoutExpired:
            return f"lldb timed out analyzing: {core_path}"
        except Exception as e:
            return f"Error: {e}"

    return await asyncio.to_thread(_run)


@mcp.tool(
    description=(
        "Poll the running example_sdl3_vulkan process via /proc and report "
        "thread state changes, new/exited threads, and process termination. "
        "Non-intrusive: safe alongside any attached debugger. "
        "interval_ms: poll interval in ms (default 500, min 100). "
        "duration_ms: total monitoring window in ms (default 5000, max 30000)."
    )
)
async def monitor_cpp_process(interval_ms: int = 500, duration_ms: int = 5000) -> str:
    """
    Watch /proc/<pid>/ and report state changes over time.
    Args:
        interval_ms: Poll interval in milliseconds (100–5000).
        duration_ms: Total monitoring window in milliseconds (100–30000).
    """
    interval_ms = max(100, min(interval_ms, 5000))
    duration_ms = max(100, min(duration_ms, 30000))
    pids = _find_cpp_pids()
    if not pids:
        return f"No '{_CPP_TARGET}' process found. Start the debug session first."
    pid = pids[0]
    events: list[str] = []
    start = _time.monotonic()
    deadline = start + duration_ms / 1000.0

    def snap() -> dict[str, str]:
        return {t["tid"]: t["state"] for t in _thread_info(pid)}

    prev = snap()
    events.append(f"[t=0ms] Monitoring PID {pid} — {len(prev)} threads")
    while _time.monotonic() < deadline:
        await asyncio.sleep(interval_ms / 1000.0)
        ms = int((_time.monotonic() - start) * 1000)
        if not _os.path.exists(f"/proc/{pid}"):
            events.append(f"[t={ms}ms] Process {pid} exited!")
            break
        cur = snap()
        for tid in sorted(set(cur) - set(prev)):
            try:
                with open(f"/proc/{pid}/task/{tid}/comm") as f:
                    name = f.read().strip()
            except OSError:
                name = "?"
            events.append(f"[t={ms}ms] +Thread TID {tid} ({name}) started")
        for tid in sorted(set(prev) - set(cur)):
            events.append(f"[t={ms}ms] -Thread TID {tid} exited")
        for tid in sorted(set(cur) & set(prev)):
            if cur[tid] != prev[tid]:
                try:
                    with open(f"/proc/{pid}/task/{tid}/comm") as f:
                        name = f.read().strip()
                except OSError:
                    name = "?"
                events.append(
                    f"[t={ms}ms]  Thread {tid} ({name}): {prev[tid]} -> {cur[tid]}"
                )
        prev = cur
    ms_total = int((_time.monotonic() - start) * 1000)
    events.append(f"[t={ms_total}ms] Done.")
    return "\n".join(events)


if __name__ == "__main__":
    mcp.run(transport="stdio")
