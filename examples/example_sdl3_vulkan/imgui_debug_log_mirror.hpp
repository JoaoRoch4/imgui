#pragma once
// imgui_debug_log_mirror.hpp
// Mirrors the Dear ImGui debug log into a file so it can be followed in
// realtime with `tail -f /tmp/imgui_debug.log`.
//
// Usage:
//   // After ImGui::CreateContext():
//   mirror.Open("/tmp/imgui_debug.log");
//   mirror.SetFlags(ImGuiDebugLogFlags_EventError |
//   ImGuiDebugLogFlags_EventActiveId);
//
//   // Each frame, right after ImGui::NewFrame():
//   mirror.Tick();
//
//   // Optionally render a window (mirrors ShowDebugLogWindow + shows the file
//   path): mirror.ShowWindow(&show_mirror);
//
//   // In shutdown, before ImGui::DestroyContext():
//   mirror.Close();

#include "imgui.h"
#include "imgui_internal.h" // g.DebugLogBuf, g.DebugLogIndex, g.DebugLogFlags

#include <cstdlib> // std::system
#include <filesystem>
#include <fstream>
#include <tuple> // std::ignore

// ---------------------------------------------------------------------------
// ImGuiDebugLogMirror
// ---------------------------------------------------------------------------
// Tails g.DebugLogBuf each frame and appends new bytes to a file.
// The file is opened with line buffering + an explicit fflush() after every
// write, so `tail -f` tracks it with no delay.
// ---------------------------------------------------------------------------
class ImGuiDebugLogMirror {
  public:
	ImGuiDebugLogMirror() : m_file{}, m_last_buf_size{0}, m_path{} {}

	// Open (or re-open) the output file.
	// Call once, after ImGui::CreateContext().
	// Returns false if the file cannot be opened.
	bool Open(const char *path) {
		Close();
		m_file.open(path, std::ios::out | std::ios::trunc);
		if (!m_file.is_open())
			return false;
		m_path = path;
		m_last_buf_size = 0;
		return true;
	}

	// Close the output file.
	void Close() {
		if (m_file.is_open())
			m_file.close();
		m_path.clear();
		m_last_buf_size = 0;
	}

	bool IsOpen() const { return m_file.is_open(); }
	const char *GetPath() const { return m_path.c_str(); }

	// Call once per frame, right after ImGui::NewFrame().
	// Writes any bytes added to g.DebugLogBuf since the last call.
	void Tick() {
		if (!m_file.is_open())
			return;
		ImGuiContext &g = *GImGui;
		const int cur = g.DebugLogBuf.size();
		if (cur <= m_last_buf_size)
			return;
		const char *begin = g.DebugLogBuf.begin() + m_last_buf_size;
		const int len = cur - m_last_buf_size;
		m_file.write(begin, static_cast<std::streamsize>(len));
		m_file.flush();
		m_last_buf_size = cur;
	}

	// Convenience: set which event categories are logged.
	// Wraps g.DebugLogFlags.  Call after ImGui::CreateContext().
	// Example: SetFlags(ImGuiDebugLogFlags_EventError |
	// ImGuiDebugLogFlags_EventActiveId)
	static void SetFlags(ImGuiDebugLogFlags flags) {
		GImGui->DebugLogFlags = flags;
	}

	static ImGuiDebugLogFlags GetFlags() { return GImGui->DebugLogFlags; }

	// Show a UI window that mirrors ImGui::ShowDebugLogWindow() and also
	// displays the path of the file being written to.
	void ShowWindow(bool *p_open = nullptr) {
		ImGuiContext &g = *GImGui;
		if ((g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize) == 0)
			ImGui::SetNextWindowSize(ImVec2(0.0f, ImGui::GetFontSize() * 14.0f),
									 ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Dear ImGui Debug Log (Mirror)", p_open) ||
			ImGui::GetCurrentWindow()->BeginCount > 1) {
			ImGui::End();
			return;
		}

		// File path info row
		if (m_file.is_open()) {
			ImGui::TextDisabled("File: %s", m_path.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Open dir")) {
				std::string cmd_str =
					"xdg-open \"" + m_path.parent_path().string() + "\" &";
				std::ignore = std::system(cmd_str.c_str());
			}
		} else {
			ImGui::TextDisabled("(no file open)");
		}

		ImGui::Separator();

		// Delegate the actual log display to the built-in window internals
		// by calling ShowDebugLogWindow on a child — but that would open a
		// second named window.  Instead we inline the log body here so the
		// flag checkboxes stay in sync with the single g.DebugLogFlags.

		// --- flag checkboxes (same as ShowDebugLogWindow) ---
		ImGuiDebugLogFlags all_flags = ImGuiDebugLogFlags_EventMask_ &
									   ~ImGuiDebugLogFlags_EventInputRouting;
		ImGui::CheckboxFlags("All", &g.DebugLogFlags, all_flags);
		ImGui::SetItemTooltip("(except InputRouting which is spammy)");

		auto FlagCheckbox = [](const char *name, ImGuiDebugLogFlags f) {
			ImGui::SameLine();
			ImGui::CheckboxFlags(name, &GImGui->DebugLogFlags, f);
		};
		FlagCheckbox("Errors", ImGuiDebugLogFlags_EventError);
		FlagCheckbox("ActiveId", ImGuiDebugLogFlags_EventActiveId);
		FlagCheckbox("Clipper", ImGuiDebugLogFlags_EventClipper);
		FlagCheckbox("Focus", ImGuiDebugLogFlags_EventFocus);
		FlagCheckbox("IO", ImGuiDebugLogFlags_EventIO);
		FlagCheckbox("Nav", ImGuiDebugLogFlags_EventNav);
		FlagCheckbox("Popup", ImGuiDebugLogFlags_EventPopup);
		FlagCheckbox("Selection", ImGuiDebugLogFlags_EventSelection);
		FlagCheckbox("InputRouting", ImGuiDebugLogFlags_EventInputRouting);

		if (ImGui::SmallButton("Clear")) {
			g.DebugLogBuf.clear();
			g.DebugLogIndex.clear();
			g.DebugLogSkippedErrors = 0;
			m_last_buf_size = 0;
			if (m_file.is_open()) {
				m_file.close();
				m_file.open(m_path, std::ios::out | std::ios::trunc);
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy"))
			ImGui::SetClipboardText(g.DebugLogBuf.c_str());

		// --- scrolling log body ---
		ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
						  ImGuiWindowFlags_AlwaysVerticalScrollbar |
							  ImGuiWindowFlags_AlwaysHorizontalScrollbar);

		const ImGuiDebugLogFlags backup = g.DebugLogFlags;
		g.DebugLogFlags &= ~ImGuiDebugLogFlags_EventClipper;

		ImGuiListClipper clipper;
		clipper.Begin(g.DebugLogIndex.size());
		while (clipper.Step())
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				ImGui::DebugTextUnformattedWithLocateItem(
					g.DebugLogIndex.get_line_begin(g.DebugLogBuf.c_str(), i),
					g.DebugLogIndex.get_line_end(g.DebugLogBuf.c_str(), i));

		g.DebugLogFlags = backup;
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
			ImGui::SetScrollHereY(1.0f);

		ImGui::EndChild();
		ImGui::End();
	}

	~ImGuiDebugLogMirror() { Close(); }

  private:
	std::ofstream         m_file;
	int                   m_last_buf_size;
	std::filesystem::path m_path;
};
