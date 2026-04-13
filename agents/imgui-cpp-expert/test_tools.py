#!/usr/bin/env python3
"""
Integration tests for imgui-cpp-expert MCP server tools.
Tests all 3 exposed tools: ask_imgui_expert, reset_session, list_sessions.

Run: python test_tools.py
"""

import asyncio
import sys
import os

# Add agent dir to path so imports work
sys.path.insert(0, os.path.dirname(__file__))

# ── helpers ────────────────────────────────────────────────────────────────────

passed = 0
failed = 0

def ok(label: str, detail: str = ""):
    global passed
    passed += 1
    detail_str = f"\n     {detail}" if detail else ""
    print(f"  \033[32mPASS\033[0m  {label}{detail_str}")

def fail(label: str, reason: str):
    global failed
    failed += 1
    print(f"  \033[31mFAIL\033[0m  {label}  →  {reason}")

def section(name: str):
    print(f"\n=== {name} ===")


# ── import server under test ───────────────────────────────────────────────────

# Import the raw functions, bypassing FastMCP decoration
# We monkey-patch _sessions access directly
import importlib.util, types

spec = importlib.util.spec_from_file_location("mcp_server", os.path.join(os.path.dirname(__file__), "mcp_server.py"))
srv = importlib.util.module_from_spec(spec)
# Pre-populate so the module-level _client and _sessions exist before exec
srv.__name__ = "mcp_server"
spec.loader.exec_module(srv)


# ── test: list_sessions ────────────────────────────────────────────────────────

async def test_list_sessions():
    section("list_sessions")

    srv._sessions.clear()
    result = srv.list_sessions()
    if isinstance(result, list) and len(result) == 0:
        ok("list_sessions() → empty list when no sessions", f"got {result!r}")
    else:
        fail("list_sessions() → empty list", f"got {result!r}")

    # Seed a session manually
    srv._sessions["alpha"] = [{"role": "user", "content": "hi"}]
    srv._sessions["beta"] = []
    result2 = srv.list_sessions()
    if set(result2) == {"alpha", "beta"}:
        ok("list_sessions() → returns all session names", f"got {result2!r}")
    else:
        fail("list_sessions() → all sessions", f"got {result2!r}")


# ── test: reset_session ────────────────────────────────────────────────────────

async def test_reset_session():
    section("reset_session")

    srv._sessions["mytest"] = [{"role": "user", "content": "hello"}]
    result = srv.reset_session("mytest")
    if isinstance(result, str) and "mytest" in result and "reset" in result.lower():
        ok("reset_session('mytest') → confirmation string", f"{result!r}")
    else:
        fail("reset_session('mytest') → confirmation string", f"got {result!r}")

    if "mytest" not in srv._sessions:
        ok("reset_session clears the session from dict")
    else:
        fail("reset_session clears from dict", "session still present")

    # Reset non-existent session (should not raise)
    try:
        r2 = srv.reset_session("nonexistent_xyz")
        ok("reset_session('nonexistent') → no error", f"{r2!r}")
    except Exception as e:
        fail("reset_session('nonexistent') → no error", str(e))


# ── test: ask_imgui_expert ─────────────────────────────────────────────────────

async def test_ask_imgui_expert():
    section("ask_imgui_expert")

    srv._sessions.clear()
    srv._client = None  # force fresh client

    # 1. Simple factual question (short answer expected)
    try:
        answer = await srv.ask_imgui_expert(
            "In one sentence: what does ImGui::Begin() return?",
            session_id="test_ask"
        )
        if isinstance(answer, str) and len(answer) > 20:
            ok("ask_imgui_expert(ImGui question) → non-empty response",
               answer[:120].replace("\n", " "))
        else:
            fail("ask_imgui_expert → non-empty response", f"got {answer!r}")
    except Exception as e:
        fail("ask_imgui_expert(ImGui question)", str(e))

    # 2. Session context kept
    if "test_ask" in srv._sessions and len(srv._sessions["test_ask"]) >= 2:
        ok("ask_imgui_expert → session history saved",
           f"{len(srv._sessions['test_ask'])} messages in 'test_ask'")
    else:
        fail("ask_imgui_expert → session history saved",
             f"sessions={list(srv._sessions.keys())}, len={len(srv._sessions.get('test_ask', []))}")

    # 3. Follow-up stays in context
    try:
        ans2 = await srv.ask_imgui_expert(
            "Any other return value?",
            session_id="test_ask"
        )
        if isinstance(ans2, str) and len(ans2) > 5:
            ok("ask_imgui_expert → follow-up works (session context)",
               ans2[:80].replace("\n", " "))
        else:
            fail("ask_imgui_expert → follow-up", f"got {ans2!r}")
    except Exception as e:
        fail("ask_imgui_expert follow-up", str(e))

    # 4. list_sessions reflects active session
    sessions = srv.list_sessions()
    if "test_ask" in sessions:
        ok("list_sessions after ask → session visible", f"{sessions}")
    else:
        fail("list_sessions after ask → session visible", f"{sessions}")

    # 5. Reset and verify
    srv.reset_session("test_ask")
    if "test_ask" not in srv._sessions:
        ok("reset_session → session removed after ask")
    else:
        fail("reset_session removes session", "still present")


# ── clang-tidy tool tests ─────────────────────────────────────────────────────

async def test_clang_tidy_tools():
    print("\n── clang-tidy tools ─────────────────────────────────")

    # 1. search_clang_tidy: known category returns results
    result = await srv.search_clang_tidy("modernize")
    if isinstance(result, str) and "modernize" in result.lower():
        ok("search_clang_tidy('modernize') → contains modernize checks")
    else:
        fail("search_clang_tidy('modernize') → contains modernize checks",
             repr(result)[:120])

    # 2. search_clang_tidy: bugprone category
    result = await srv.search_clang_tidy("bugprone")
    if isinstance(result, str) and "bugprone" in result.lower():
        ok("search_clang_tidy('bugprone') → contains bugprone checks")
    else:
        fail("search_clang_tidy('bugprone') → contains bugprone checks",
             repr(result)[:120])

    # 3. search_clang_tidy: no-match returns informative message (not an exception)
    result = await srv.search_clang_tidy("zzz_nonexistent_xyzzy_check")
    if isinstance(result, str) and ("not found" in result.lower() or "no " in result.lower()):
        ok("search_clang_tidy(nonexistent) → informative 'not found' message")
    else:
        fail("search_clang_tidy(nonexistent) → informative 'not found' message",
             repr(result)[:120])

    # 4. lookup_clang_tidy: well-known check returns non-empty docs
    result = await srv.lookup_clang_tidy("modernize-use-nullptr")
    if isinstance(result, str) and len(result) > 100 and "modernize-use-nullptr" in result:
        ok("lookup_clang_tidy('modernize-use-nullptr') → non-empty documentation")
    else:
        fail("lookup_clang_tidy('modernize-use-nullptr') → non-empty documentation",
             repr(result)[:120])

    # 5. lookup_clang_tidy: doc text contains URL
    if isinstance(result, str) and "clang.llvm.org" in result:
        ok("lookup_clang_tidy result contains canonical URL")
    else:
        fail("lookup_clang_tidy result contains canonical URL", repr(result)[:80])

    # 6. lookup_clang_tidy: unknown check returns a clear error (not an exception)
    result = await srv.lookup_clang_tidy("zzznonexistent-check-name")
    if isinstance(result, str) and (
        "cannot resolve" in result.lower() or "failed" in result.lower()
        or "not resolve" in result.lower()
    ):
        ok("lookup_clang_tidy(unknown) → clear error message")
    else:
        fail("lookup_clang_tidy(unknown) → clear error message", repr(result)[:120])

    # 7. interpret_clang_tidy: standard diagnostic with [check-name]
    diag = "warning: use std::make_unique instead [modernize-make-unique]"
    result = await srv.interpret_clang_tidy(diag)
    if (
        isinstance(result, str)
        and "modernize-make-unique" in result
        and "clang.llvm.org" in result
    ):
        ok("interpret_clang_tidy: extracts check name + URL from standard diagnostic")
    else:
        fail("interpret_clang_tidy: extracts check name + URL", repr(result)[:120])

    # 8. interpret_clang_tidy: diagnostic with path prefix stripped
    diag2 = "/home/user/test.cpp:42:10: warning: pointer arithmetic [cppcoreguidelines-pro-bounds-pointer-arithmetic]"
    result2 = await srv.interpret_clang_tidy(diag2)
    if isinstance(result2, str) and "cppcoreguidelines-pro-bounds-pointer-arithmetic" in result2:
        ok("interpret_clang_tidy: handles 'file:line:col: warning:' prefix")
    else:
        fail("interpret_clang_tidy: handles file:line:col prefix", repr(result2)[:120])

    # 9. interpret_clang_tidy: diagnostic with no [check-name] → clear error
    result3 = await srv.interpret_clang_tidy("warning: something went wrong here")
    if isinstance(result3, str) and (
        "no clang-tidy" in result3.lower() or "not found" in result3.lower()
        or "expected" in result3.lower()
    ):
        ok("interpret_clang_tidy: no [check-name] → informative error")
    else:
        fail("interpret_clang_tidy: no [check-name] → informative error", repr(result3)[:120])


# ── process / debugger tool tests ───────────────────────────────────────────

async def test_process_tools():
    print("\n── process / debugger tools ─────────────────────────────")

    # get_cpp_process_state — no process running: expect informative message
    result = srv.get_cpp_process_state()
    if isinstance(result, str) and len(result) > 5:
        if "example_sdl3_vulkan" in result:
            ok("get_cpp_process_state() → returns a string")
        else:
            fail("get_cpp_process_state() → mentions target process", repr(result)[:120])
    else:
        fail("get_cpp_process_state() → returns a string", repr(result)[:80])

    # get_cpp_process_state — no-process path returns error message (not an exception)
    if "No" in result or "Process PID" in result:
        ok("get_cpp_process_state() → graceful no-process message")
    else:
        fail("get_cpp_process_state() → graceful no-process message", repr(result)[:120])

    # get_cpp_thread_backtraces — no process (pid=0 auto-detect)
    try:
        result2 = await srv.get_cpp_thread_backtraces(pid=0)
        if isinstance(result2, str) and len(result2) > 5:
            ok("get_cpp_thread_backtraces(pid=0) → returns a string",
               result2[:80].replace("\n", " "))
        else:
            fail("get_cpp_thread_backtraces(pid=0) → returns a string", repr(result2)[:80])
    except Exception as e:
        fail("get_cpp_thread_backtraces(pid=0) → no exception", str(e))

    # analyze_core_dump — no core file: expect informative message (not an exception)
    try:
        result3 = await srv.analyze_core_dump(core_path="")
        if isinstance(result3, str) and (
            "core" in result3.lower() or "not found" in result3.lower()
            or "ulimit" in result3.lower()
        ):
            ok("analyze_core_dump('') → informative no-core message")
        else:
            fail("analyze_core_dump('') → informative no-core message", repr(result3)[:120])
    except Exception as e:
        fail("analyze_core_dump('') → no exception", str(e))

    # analyze_core_dump — nonexistent explicit path
    try:
        result4 = await srv.analyze_core_dump(core_path="/nonexistent/core.dump")
        if isinstance(result4, str) and (
            "not found" in result4.lower() or "no such" in result4.lower()
            or "/nonexistent" in result4
        ):
            ok("analyze_core_dump(nonexistent path) → file-not-found message")
        else:
            fail("analyze_core_dump(nonexistent path) → file-not-found message", repr(result4)[:120])
    except Exception as e:
        fail("analyze_core_dump(nonexistent path) → no exception", str(e))

    # monitor_cpp_process — no process: should return immediately with error message
    try:
        result5 = await srv.monitor_cpp_process(interval_ms=100, duration_ms=200)
        if isinstance(result5, str) and len(result5) > 5:
            ok("monitor_cpp_process() → returns a string",
               result5[:80].replace("\n", " "))
        else:
            fail("monitor_cpp_process() → returns a string", repr(result5)[:80])
    except Exception as e:
        fail("monitor_cpp_process() → no exception", str(e))


# ── main ──────────────────────────────────────────────────────────────────────

async def main():
    print("imgui-cpp-expert MCP server — tool integration tests")
    print("=" * 50)

    await test_list_sessions()
    await test_reset_session()
    await test_ask_imgui_expert()
    await test_clang_tidy_tools()
    await test_process_tools()

    total = passed + failed
    print("\n" + "─" * 50)
    print(f"  {total} tests:  {passed} passed,  {failed} failed")

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
