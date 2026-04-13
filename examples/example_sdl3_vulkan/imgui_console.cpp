#include "imgui_console.hpp"

#include <algorithm>   // std::max
#include <atomic>      // std::atomic
#include <cctype>      // toupper
#include <cerrno>      // errno
#include <cstdarg>     // va_list
#include <cstdio>      // FILE, popen, pclose, fgets, sprintf
#include <cstring>     // strlen, strcpy, strstr, strncmp, memcpy, strerror
#include <fcntl.h>     // posix_openpt, O_RDWR, O_NOCTTY
#include <fstream>     // std::ifstream
#include <memory>      // std::shared_ptr
#include <mutex>       // std::mutex, std::lock_guard
#include <poll.h>      // poll, pollfd
#include <string>      // std::string, std::stoi
#include <sys/ioctl.h> // ioctl, TIOCSCTTY
#include <sys/types.h> // pid_t
#include <sys/wait.h>  // waitpid, WIFEXITED, WEXITSTATUS, WIFSIGNALED, WTERMSIG
#include <thread>      // std::thread
#include <unistd.h>    // fork, setsid, dup2, close, read, write, execl, _exit
#include <vector>      // std::vector

// ══════════════════════════════════════════════════════════════════════════════
// BashSession — PTY state shared between main thread and worker thread
// ══════════════════════════════════════════════════════════════════════════════

struct BashSession {
	std::atomic<int> master_fd{-1};       // master PTY fd; -1 = already closed
	std::atomic<bool> running{true};      // worker thread is alive
	std::atomic<bool> needs_input{false}; // password prompt has been detected
	std::atomic<bool> terminal_mode{
		false};               // interactive TTY: forward all input
	std::array<char, 256> password_buf{}; // filled by the main thread on Enter
	std::mutex fd_mutex;      // serialises write (main) vs close (worker)
};

// ══════════════════════════════════════════════════════════════════════════════
// ImGuiConsole — core
// ══════════════════════════════════════════════════════════════════════════════

ImGuiConsole::ImGuiConsole()
	: InputBuf{}
	, HistoryPos{-1}
	, AutoScroll{true}
	, ScrollToBottom{false}
	, SelectedItem_{}
	, Alive_{std::make_shared<std::atomic<bool>>(true)}
	, BashJobCount_{0}
{
	AddLog("Console ready. Type HELP for a list of commands.\n");
}

ImGuiConsole::~ImGuiConsole() {
	// Signal any running background threads that the console is gone.
	*Alive_ = false;
	ClearLog();
}

// ─── ClearLog ────────────────────────────────────────────────────────────────

void ImGuiConsole::ClearLog() {
	Items.clear();
	SelectedItem_.clear();
}

// ─── AddLog ──────────────────────────────────────────────────────────────────

void ImGuiConsole::AddLog(const char *fmt, ...) {
	std::array<char, 4096> buf{};
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf.data(), buf.size(), fmt, args);
	buf.at(buf.size() - 1) = '\0';
	va_end(args);
	Items.emplace_back(buf.data());
}

// ─── RegisterCommand ─────────────────────────────────────────────────────────

void ImGuiConsole::RegisterCommand(const char *name, const char *description,
								   ConsoleCommandFn fn) {
	ConsoleCommandDef def;
	def.description = description;
	def.fn = std::move(fn);
	def.name = name;
	for (char &c : def.name)
		c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
	Commands.push_back(std::move(def));
}

// ─── ExecCommand ─────────────────────────────────────────────────────────────

void ImGuiConsole::ExecCommand(const char *command_line) {
	AddLog("# %s\n", command_line);

	// ── History (deduplicate, most-recent at end)
	// ─────────────────────────────
	HistoryPos = -1;
	for (int i = static_cast<int>(History.size()) - 1; i >= 0; i--)
		if (Stricmp(History.at(i).c_str(), command_line) == 0) {
			History.erase(History.begin() + i);
			break;
		}
	History.emplace_back(command_line);

	// ── Tokenise on whitespace
	// ────────────────────────────────────────────────
	std::vector<std::string> tokens;
	{
		const char *p = command_line;
		while (*p) {
			while (*p == ' ' || *p == '\t')
				++p;
			if (!*p)
				break;
			const char *start = p;
			while (*p && *p != ' ' && *p != '\t')
				++p;
			tokens.emplace_back(start, p - start);
		}
	}
	if (tokens.empty())
		return;

	// Upper-case command name
	for (char &c : tokens.at(0))
		c = static_cast<char>(toupper(static_cast<unsigned char>(c)));

	// raw_args: everything after the command token, leading whitespace stripped
	const char *raw_start = command_line;
	while (*raw_start && *raw_start != ' ' && *raw_start != '\t')
		++raw_start;
	while (*raw_start == ' ' || *raw_start == '\t')
		++raw_start;

	std::vector<std::string> args_vec(tokens.begin() + 1, tokens.end());

	ConsoleCommandArgs cargs{std::string_view(tokens.at(0)), std::move(args_vec),
							 std::string_view(raw_start)};

	// ── Dispatch
	// ──────────────────────────────────────────────────────────────
	for (auto &cmd : Commands) {
		if (cmd.name == tokens.at(0)) {
			cmd.fn(*this, cargs);
			ScrollToBottom = true;
			return;
		}
	}

	AddLog("[error] Unknown command '%s'. Type HELP for a list.\n",
		   tokens.at(0).c_str());
	ScrollToBottom = true;
}

// ─── Draw ────────────────────────────────────────────────────────────────────

// ─── AddLogThreadSafe ────────────────────────────────────────────────────────

void ImGuiConsole::AddLogThreadSafe(std::string line) {
	std::lock_guard<std::mutex> lk(PendingMutex_);
	PendingLines_.push_back(std::move(line));
}

// ─── FlushPendingLogs
// ─────────────────────────────────────────────────────────

void ImGuiConsole::FlushPendingLogs() {
	std::vector<std::string> tmp;
	{
		std::lock_guard<std::mutex> lk(PendingMutex_);
		tmp.swap(PendingLines_);
	}
	for (auto &line : tmp)
		AddLog("%s", line.c_str());
}

// ─── Draw
// ─────────────────────────────────────────────────────────────────────

void ImGuiConsole::DrawContents(const char *id) {
	FlushPendingLogs();
	ImGui::PushID(id);

	// ── Toolbar
	// ───────────────────────────────────────────────────────────────
	if (ImGui::SmallButton("Clear"))
		ClearLog();
	ImGui::SameLine();
	bool copy_to_clipboard = ImGui::SmallButton("Copy");
	ImGui::SameLine(0, 12);
	ImGui::Text("Filter:");
	ImGui::SameLine();
	Filter.Draw("##filter", 180);
	ImGui::SameLine();
	if (ImGui::SmallButton("X"))
		Filter.Clear();
	ImGui::SameLine();
	ImGui::Checkbox("Auto-scroll", &AutoScroll);

	ImGui::Separator();

	// ── Scrolling log region
	// ──────────────────────────────────────────────────
	const float footer =
		ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
	if (ImGui::BeginChild("##log", ImVec2(0, -footer), ImGuiChildFlags_None,
						  ImGuiWindowFlags_HorizontalScrollbar)) {
		if (ImGui::BeginPopupContextWindow()) {
			if (ImGui::Selectable("Clear"))
				ClearLog();
			ImGui::EndPopup();
		}

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
		if (copy_to_clipboard)
			ImGui::LogToClipboard();

		int display_idx = 0;
		for (const auto &item : Items) {
			if (!Filter.PassFilter(item.c_str()))
				continue;

			ImVec4 color = {};
			bool has_color = false;
			if (std::strstr(item.c_str(), "[error]")) {
				color = {1.0f, 0.4f, 0.4f, 1.0f};
				has_color = true;
			} else if (std::strstr(item.c_str(), "[warn]")) {
				color = {1.0f, 1.0f, 0.4f, 1.0f};
				has_color = true;
			} else if (std::strncmp(item.c_str(), "# ", 2) == 0) {
				color = {1.0f, 0.8f, 0.6f, 1.0f};
				has_color = true;
			} else if (std::strncmp(item.c_str(), "$ ", 2) == 0) {
				color = {0.4f, 1.0f, 0.4f, 1.0f};
				has_color = true;
			}

			bool is_selected = (item == SelectedItem_);
			ImGui::PushID(display_idx++);
			if (item.at(0) == '\x01') {
				// Two-column command listing: \x01NAME\tDESCRIPTION
				const char *tab = std::strchr(item.c_str() + 1, '\t');
				if (tab) {
					ImGui::TextColored({1.0f, 1.0f, 0.0f, 1.0f}, "%.*s",
									   static_cast<int>(tab - (item.c_str() + 1)), item.c_str() + 1);
					ImGui::SameLine(120.0f);
					const char *desc = tab + 1;
					size_t dl = std::strlen(desc);
					while (dl > 0 && (desc[dl-1] == '\n' || desc[dl-1] == '\r')) --dl;
					ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "%.*s",
									   static_cast<int>(dl), desc);
				} else {
					ImGui::TextColored({1.0f, 1.0f, 0.0f, 1.0f}, "%s", item.c_str() + 1);
				}
			} else {
				if (has_color)
					ImGui::PushStyleColor(ImGuiCol_Text, color);
				if (ImGui::Selectable(item.c_str(), is_selected,
									  ImGuiSelectableFlags_None)) {
					SelectedItem_ = item;
					ImGui::SetClipboardText(item.c_str());
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Click to copy");
				if (has_color)
					ImGui::PopStyleColor();
			}
			ImGui::PopID();
		}

		if (copy_to_clipboard)
			ImGui::LogFinish();

		if (ScrollToBottom ||
			(AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
			ImGui::SetScrollHereY(1.0f);
		ScrollToBottom = false;

		ImGui::PopStyleVar();
	}
	ImGui::EndChild();

	ImGui::Separator();

	// ── Input line
	// ──────────────────────────────────────────────────────────── When an
	// active BASH job needs a password we switch the input field into masked
	// mode and route text directly to the process PTY instead of the command
	// dispatcher.
	bool reclaim_focus = false;

	std::shared_ptr<BashSession> active_session;
	{
		std::lock_guard<std::mutex> lk(BashSessionMutex_);
		active_session = ActiveBashSession_;
	}

	if (active_session && active_session->terminal_mode.load()) {
		// ── Terminal pass-through mode
		// ─────────────────────────────────────────
		ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "[TTY]");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##tty_input", InputBuf.data(), InputBuf.size(),
							 ImGuiInputTextFlags_EnterReturnsTrue |
								 ImGuiInputTextFlags_EscapeClearsAll)) {
			std::string line(InputBuf.data());
			line += '\n';
			{
				std::lock_guard<std::mutex> lk(active_session->fd_mutex);
				int fd = active_session->master_fd.load();
				if (fd >= 0)
					write(fd, line.c_str(), line.size());
			}
			InputBuf.at(0) = '\0';
			reclaim_focus = true;
		}
		ImGui::SetItemDefaultFocus();
		if (reclaim_focus)
			ImGui::SetKeyboardFocusHere(-1);
	} else if (active_session && active_session->needs_input.load()) {
		// ── Password mode
		// ─────────────────────────────────────────────────────
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(password)");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##pw_input", active_session->password_buf.data(),
							 active_session->password_buf.size(),
							 ImGuiInputTextFlags_EnterReturnsTrue |
								 ImGuiInputTextFlags_Password |
								 ImGuiInputTextFlags_EscapeClearsAll)) {
			// Send password + newline to child process via PTY
			std::string pw(active_session->password_buf.data());
			pw += '\n';
			{
				std::lock_guard<std::mutex> lk(active_session->fd_mutex);
				int fd = active_session->master_fd.load();
				if (fd >= 0)
					write(fd, pw.c_str(), pw.size());
			}
			active_session->password_buf.fill(0);
			active_session->needs_input.store(false);
			reclaim_focus = true;
		}
		ImGui::SetItemDefaultFocus();
		if (reclaim_focus)
			ImGui::SetKeyboardFocusHere(-1);
	} else {
		// ── Normal command mode
		// ───────────────────────────────────────────────
		ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
									ImGuiInputTextFlags_EscapeClearsAll |
									ImGuiInputTextFlags_CallbackCompletion |
									ImGuiInputTextFlags_CallbackHistory;
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##input", InputBuf.data(), InputBuf.size(), flags,
							 &TextEditCallbackStub, this)) {
			Strtrim(InputBuf.data());
			if (InputBuf.at(0))
				ExecCommand(InputBuf.data());
			InputBuf.at(0) = '\0';
			reclaim_focus = true;
		}
		ImGui::SetItemDefaultFocus();
		if (reclaim_focus)
			ImGui::SetKeyboardFocusHere(-1);
	}

	ImGui::PopID();
}

void ImGuiConsole::Draw(const char *title, bool *p_open) {
	ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(title, p_open)) {
		ImGui::End();
		return;
	}
	std::array<char, 24> uid{};
	std::snprintf(uid.data(), uid.size(), "%p", static_cast<void *>(this));
	DrawContents(uid.data());
	ImGui::End();
}

// ─── TextEditCallback
// ─────────────────────────────────────────────────────────

int ImGuiConsole::TextEditCallbackStub(ImGuiInputTextCallbackData *data) {
	return static_cast<ImGuiConsole *>(data->UserData)->TextEditCallback(data);
}

int ImGuiConsole::TextEditCallback(ImGuiInputTextCallbackData *data) {
	if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
		// Identify the word being completed
		const char *word_end = data->Buf + data->CursorPos;
		const char *word_start = word_end;
		while (word_start > data->Buf) {
			char c = word_start[-1];
			if (c == ' ' || c == '\t' || c == ',' || c == ';')
				break;
			--word_start;
		}

		// Collect matching commands
		std::vector<int> candidates; // indices into Commands
		for (int ci = 0; ci < static_cast<int>(Commands.size()); ++ci)
				if (Strnicmp(Commands.at(ci).name.c_str(), word_start,
						 static_cast<int>(word_end - word_start)) == 0)
				candidates.push_back(ci);

		if (candidates.size() == 0) {
			AddLog("No completion for \"%.*s\"\n", static_cast<int>(word_end - word_start),
				   word_start);
		} else if (candidates.size() == 1) {
			data->DeleteChars(static_cast<int>(word_start - data->Buf),
							  static_cast<int>(word_end - word_start));
			data->InsertChars(data->CursorPos, Commands.at(candidates.at(0)).name.c_str());
			data->InsertChars(data->CursorPos, " ");
			AddLog("\x01%s\t%s\n",
				   Commands.at(candidates.at(0)).name.c_str(),
				   Commands.at(candidates.at(0)).description.c_str());
		} else {
			// Expand to longest common prefix then list candidates
			int match_len = static_cast<int>(word_end - word_start);
			for (;;) {
				int c = 0;
				bool all_match = true;
			for (int i = 0; i < static_cast<int>(candidates.size()) && all_match; ++i) {
					const char ch = Commands.at(static_cast<std::size_t>(candidates.at(static_cast<std::size_t>(i)))).name.at(static_cast<std::size_t>(match_len));
					if (i == 0)
						c = toupper(static_cast<unsigned char>(ch));
					else if (c == 0 || c != toupper(static_cast<unsigned char>(ch)))
						all_match = false;
				}
				if (!all_match)
					break;
				++match_len;
			}
			if (match_len > static_cast<int>(word_end - word_start)) {
				data->DeleteChars(static_cast<int>(word_start - data->Buf),
								  static_cast<int>(word_end - word_start));
				const char *first = Commands.at(candidates.at(0)).name.c_str();
				data->InsertChars(data->CursorPos, first, first + match_len);
			}
			AddLog("Possible completions:\n");
			for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
				AddLog("\x01%s\t%s\n",
					   Commands.at(candidates.at(i)).name.c_str(),
					   Commands.at(candidates.at(i)).description.c_str());
		}
	} else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
		const int prev = HistoryPos;
		if (data->EventKey == ImGuiKey_UpArrow) {
			if (HistoryPos == -1)
				HistoryPos = static_cast<int>(History.size()) - 1;
			else if (HistoryPos > 0)
				--HistoryPos;
		} else if (data->EventKey == ImGuiKey_DownArrow) {
			if (HistoryPos != -1)
				if (++HistoryPos >= static_cast<int>(History.size()))
					HistoryPos = -1;
		}
		if (prev != HistoryPos) {
			const char *entry =
				HistoryPos >= 0
					? History.at(static_cast<std::size_t>(HistoryPos)).c_str()
					: "";
			data->DeleteChars(0, data->BufTextLen);
			data->InsertChars(0, entry);
		}
	}
	return 0;
}

// ─── Static helpers ──────────────────────────────────────────────────────────

int ImGuiConsole::Stricmp(const char *s1, const char *s2) {
	int d{};
	while ((d = toupper(static_cast<unsigned char>(*s2)) - toupper(static_cast<unsigned char>(*s1))) ==
			   0 &&
		   *s1)
		++s1, ++s2;
	return d;
}

int ImGuiConsole::Strnicmp(const char *s1, const char *s2, int n) {
	int d = 0;
	while (n > 0 &&
		   (d = toupper(static_cast<unsigned char>(*s2)) - toupper(static_cast<unsigned char>(*s1))) ==
			   0 &&
		   *s1)
		--n, ++s1, ++s2;
	return d;
}

void ImGuiConsole::Strtrim(char *s) {
	char *end = s + std::strlen(s);
	while (end > s && end[-1] == ' ')
		--end;
	*end = '\0';
}

// ══════════════════════════════════════════════════════════════════════════════
// ConsoleCommands — constructor: register all built-in commands
// ══════════════════════════════════════════════════════════════════════════════

ConsoleCommands::ConsoleCommands() {
	// Bind a member function pointer to a ConsoleCommandFn compatible lambda.
	auto bind = [this](void (ConsoleCommands::*m)(const ConsoleCommandArgs &)) {
		return [this, m](ImGuiConsole & /*c*/, const ConsoleCommandArgs &a) {
			(this->*m)(a);
		};
	};

	RegisterCommand("HELP", "List all registered commands",
					bind(&ConsoleCommands::CmdHelp));
	RegisterCommand("HISTORY", "Print the last 10 history entries",
					bind(&ConsoleCommands::CmdHistory));
	RegisterCommand("CLEAR", "Erase the console log",
					bind(&ConsoleCommands::CmdClear));
	RegisterCommand("ECHO", "Echo text: ECHO <text…>",
					bind(&ConsoleCommands::CmdEcho));
	RegisterCommand("FPS", "Print current framerate",
					bind(&ConsoleCommands::CmdFps));
	RegisterCommand("STYLE", "Switch theme: STYLE DARK|LIGHT|CLASSIC",
					bind(&ConsoleCommands::CmdStyle));
	RegisterCommand("DEMO", "Toggle demo window: DEMO ON|OFF",
					bind(&ConsoleCommands::CmdDemo));
	RegisterCommand("LOG", "Tail debug log: LOG [n=20]",
					bind(&ConsoleCommands::CmdLog));
	RegisterCommand("SET", "Set variable: SET <name> <value…>",
					bind(&ConsoleCommands::CmdSet));
	RegisterCommand("GET", "Get variable: GET [name]",
					bind(&ConsoleCommands::CmdGet));
	RegisterCommand("BASH", "Run shell command: BASH <cmd> [args\u2026]",
					bind(&ConsoleCommands::CmdBash));
	RegisterCommand("COPILOT",
					"Ask GitHub Copilot CLI: COPILOT <question\u2026>",
					bind(&ConsoleCommands::CmdCopilot));
	RegisterCommand("TERMINAL",
					"Open interactive shell ($SHELL) in the console",
					bind(&ConsoleCommands::CmdTerminal));
	RegisterCommand("KONSOLE", "Alias for TERMINAL",
					bind(&ConsoleCommands::CmdTerminal));
	RegisterCommand("QUIT", "Exit the application",
					bind(&ConsoleCommands::CmdQuit));
}

// ── HELP ─────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdHelp(const ConsoleCommandArgs & /*a*/) {
	AddLog("Available commands:\n");
	for (const auto &cmd : Commands)
		AddLog("\x01%s\t%s\n", cmd.name.c_str(), cmd.description.c_str());
}

// ── HISTORY ──────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdHistory(const ConsoleCommandArgs & /*a*/) {
	int first = static_cast<int>(History.size()) - 10;
	for (int i = (first < 0 ? 0 : first); i < static_cast<int>(History.size()); ++i)
		AddLog("%3d: %s\n", i, History.at(static_cast<std::size_t>(i)).c_str());
}

// ── CLEAR ────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdClear(const ConsoleCommandArgs & /*a*/) { ClearLog(); }

// ── ECHO ─────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdEcho(const ConsoleCommandArgs &a) {
	if (a.raw_args.empty())
		AddLog("\n");
	else
		AddLog("%.*s\n", static_cast<int>(a.raw_args.size()), a.raw_args.data());
}

// ── FPS ──────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdFps(const ConsoleCommandArgs & /*a*/) {
	const ImGuiIO &io = ImGui::GetIO();
	AddLog("%.1f FPS  (%.3f ms/frame)\n", io.Framerate, 1000.0f / io.Framerate);
}

// ── STYLE ────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdStyle(const ConsoleCommandArgs &a) {
	if (a.args.empty()) {
		AddLog("[error] Usage: STYLE DARK|LIGHT|CLASSIC\n");
		return;
	}
	std::string which = a.args.at(0);
	for (char &c : which)
		c = static_cast<char>(toupper(static_cast<unsigned char>(c)));

	if (which == "DARK") {
		ImGui::StyleColorsDark();
		if (OnStyleChange)
			OnStyleChange(0);
		AddLog("Style: Dark\n");
	} else if (which == "LIGHT") {
		ImGui::StyleColorsLight();
		if (OnStyleChange)
			OnStyleChange(1);
		AddLog("Style: Light\n");
	} else if (which == "CLASSIC") {
		ImGui::StyleColorsClassic();
		if (OnStyleChange)
			OnStyleChange(2);
		AddLog("Style: Classic\n");
	} else
		AddLog("[error] Unknown style '%s'. Use DARK, LIGHT or CLASSIC.\n",
			   a.args.at(0).c_str());
}

// ── DEMO ─────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdDemo(const ConsoleCommandArgs &a) {
	if (a.args.empty()) {
		AddLog("[error] Usage: DEMO ON|OFF\n");
		return;
	}
	std::string which = a.args.at(0);
	for (char &c : which)
		c = static_cast<char>(toupper(static_cast<unsigned char>(c)));

	if (which == "ON") {
		if (OnDemoToggle)
			OnDemoToggle(true);
		AddLog("Demo window: ON\n");
	} else if (which == "OFF") {
		if (OnDemoToggle)
			OnDemoToggle(false);
		AddLog("Demo window: OFF\n");
	} else
		AddLog("[error] Unknown argument '%s'. Use ON or OFF.\n",
			   a.args.at(0).c_str());
}

// ── LOG ──────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdLog(const ConsoleCommandArgs &a) {
	int n = 20;
	if (!a.args.empty()) {
		try {
			n = std::stoi(a.args.at(0));
		} catch (...) {
			AddLog("[error] LOG: invalid line count\n");
			return;
		}
	}
	if (n <= 0 || n > 1000) {
		AddLog("[error] LOG: line count must be 1-1000\n");
		return;
	}

	std::ifstream f("/tmp/imgui_debug.log");
	if (!f) {
		AddLog("[error] Cannot open /tmp/imgui_debug.log\n");
		return;
	}

	std::vector<std::string> lines;
	lines.reserve(256);
	std::string line;
	while (std::getline(f, line))
		lines.push_back(std::move(line));

	int start = std::max(0, static_cast<int>(lines.size()) - n);
	AddLog("── /tmp/imgui_debug.log (last %d lines) ──\n",
		   static_cast<int>(lines.size()) - start);
	for (int i = start; i < static_cast<int>(lines.size()); ++i)
		AddLog("%s\n", lines.at(static_cast<std::size_t>(i)).c_str());
}

// ── SET ──────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdSet(const ConsoleCommandArgs &a) {
	if (a.args.size() < 2) {
		AddLog("[error] Usage: SET <name> <value>\n");
		return;
	}
	std::string value;
	for (size_t i = 1; i < a.args.size(); ++i) {
		if (i > 1)
			value += ' ';
		value += a.args.at(i);
	}
	Variables.insert_or_assign(a.args.at(0), value);
	AddLog("SET %s = %s\n", a.args.at(0).c_str(), value.c_str());
}

// ── GET ──────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdGet(const ConsoleCommandArgs &a) {
	if (a.args.empty()) {
		if (Variables.empty()) {
			AddLog("(no variables set)\n");
			return;
		}
		for (const auto &[k, v] : Variables)
			AddLog("  %s = %s\n", k.c_str(), v.c_str());
		return;
	}
	auto it = Variables.find(a.args.at(0));
	if (it == Variables.end())
		AddLog("[error] Undefined variable '%s'\n", a.args.at(0).c_str());
	else
		AddLog("%s = %s\n", a.args.at(0).c_str(), it->second.c_str());
}

// ── QUIT ─────────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdQuit(const ConsoleCommandArgs & /*a*/) {
	AddLog("Quitting…\n");
	if (OnQuit)
		OnQuit();
}

// ── BASH ─────────────────────────────────────────────────────────────────────
// Opens a PTY, forks /bin/sh with the PTY slave as its controlling terminal,
// and reads output on a detached thread.  Because the child sees a real TTY,
// tools like sudo can prompt for a password; the prompt is detected in the
// output stream and the console input switches to masked mode so the user can
// type the password without it appearing in the log.
//
// This is intentionally developer-only — it runs with the same privileges as
// the parent process.  Do not expose to untrusted input.

void ConsoleCommands::CmdBash(const ConsoleCommandArgs &a) {
	if (a.raw_args.empty()) {
		AddLog("[error] Usage: BASH <shell command>\n");
		return;
	}

	std::string cmd(a.raw_args);
	std::shared_ptr<std::atomic<bool>> alive = Alive_;

	// ── Open master PTY
	// ───────────────────────────────────────────────────────
	int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (master_fd < 0) {
		AddLog("[error] posix_openpt: %s\n", strerror(errno));
		return;
	}

	if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
		close(master_fd);
		AddLog("[error] grantpt/unlockpt: %s\n", strerror(errno));
		return;
	}

	std::array<char, 256> slave_name{};
	if (ptsname_r(master_fd, slave_name.data(), slave_name.size()) != 0) {
		close(master_fd);
		AddLog("[error] ptsname_r: %s\n", strerror(errno));
		return;
	}

	// ── Create and register the session record
	// ────────────────────────────────
	auto session = std::make_shared<BashSession>();
	session->master_fd.store(master_fd);
	{
		std::lock_guard<std::mutex> lk(BashSessionMutex_);
		ActiveBashSession_ = session;
	}

	AddLog("$ %s\n", cmd.c_str());
	++BashJobCount_;

	// ── Fork child
	// ────────────────────────────────────────────────────────────
	pid_t pid = fork();
	if (pid < 0) {
		close(master_fd);
		session->master_fd.store(-1);
		AddLog("[error] fork: %s\n", strerror(errno));
		{
			std::lock_guard<std::mutex> lk(BashSessionMutex_);
			ActiveBashSession_.reset();
		}
		--BashJobCount_;
		return;
	}

	if (pid == 0) {
		// ── Child: set PTY slave as controlling terminal, then exec
		// ───────────
		setsid();
		int slave_fd = open(slave_name.data(), O_RDWR);
		if (slave_fd < 0)
			_exit(127);
		ioctl(slave_fd, TIOCSCTTY, 0);
		dup2(slave_fd, STDIN_FILENO);
		dup2(slave_fd, STDOUT_FILENO);
		dup2(slave_fd, STDERR_FILENO);
		if (slave_fd > STDERR_FILENO)
			close(slave_fd);
		close(master_fd);
		execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
		_exit(127);
	}

	// ── Worker thread: read PTY output and detect password prompts
	// ────────────
	std::thread worker([this, session, alive, pid]() {
		std::array<char, 512> buf{};
		std::string partial; // accumulates bytes before the next newline

		while (true) {
			if (!alive->load())
				break;

			int fd = session->master_fd.load();
			if (fd < 0)
				break;

			// Wait up to 50 ms so we can check the alive flag regularly.
			struct pollfd pfd{fd, POLLIN, 0};
			int ret = poll(&pfd, 1, 50);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (ret == 0)
				continue;
			if (pfd.revents & (POLLHUP | POLLERR))
				break;

			ssize_t n = read(fd, buf.data(), buf.size() - 1);
			if (n <= 0)
				break; // EIO = child closed the PTY slave
			buf.at(static_cast<std::size_t>(n)) = '\0';

			// PTY ONLCR flag translates \n → \r\n; strip the \r.
			for (ssize_t i = 0; i < n; ++i)
				if (buf.at(static_cast<std::size_t>(i)) != '\r')
					partial += buf.at(static_cast<std::size_t>(i));

			// Flush every complete line to the log.
			size_t pos{};
			while ((pos = partial.find('\n')) != std::string::npos) {
				if (alive->load())
					AddLogThreadSafe(partial.substr(0, pos) + "\n");
				partial.erase(0, pos + 1);
			}

			// Detect password / passphrase prompts.
			// sudo writes "[sudo] password for user: " with no trailing
			// newline.
			if (!partial.empty() && !session->needs_input.load()) {
				std::string lower = partial;
				for (char &c : lower)
					c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
				bool looks_like_prompt =
					(lower.find("password") != std::string::npos ||
					 lower.find("passphrase") != std::string::npos) &&
					(partial.back() == ':' || partial.back() == ' ');

				if (looks_like_prompt && alive->load()) {
					AddLogThreadSafe(partial + "\n");
					partial.clear();
					// Signal the main thread to switch the input to password
					// mode.
					session->needs_input.store(true);
				}
			}
		}

		// Flush any buffered tail without a trailing newline.
		if (!partial.empty() && alive->load())
			AddLogThreadSafe(partial + "\n");

		// Reap the child.
		int status = 0;
		waitpid(pid, &status, 0);
		if (alive->load()) {
			if (WIFEXITED(status) && WEXITSTATUS(status))
				AddLogThreadSafe("[exit " +
								 std::to_string(WEXITSTATUS(status)) + "]\n");
			else if (WIFSIGNALED(status))
				AddLogThreadSafe("[signal " + std::to_string(WTERMSIG(status)) +
								 "]\n");
		}

		// Close master fd under lock so the main thread can't write after
		// close.
		{
			std::lock_guard<std::mutex> lk(session->fd_mutex);
			int fd = session->master_fd.exchange(-1);
			if (fd >= 0)
				close(fd);
		}
		session->running.store(false);
		session->needs_input.store(false);

		{
			std::lock_guard<std::mutex> lk(BashSessionMutex_);
			if (ActiveBashSession_ == session)
				ActiveBashSession_.reset();
		}

		if (alive->load())
			--BashJobCount_;
	});
	worker.detach();
}

// ── COPILOT
// ───────────────────────────────────────────────────────────────────

void ConsoleCommands::CmdCopilot(const ConsoleCommandArgs &a) {
	if (a.raw_args.empty()) {
		AddLog("[error] Usage: COPILOT <question>\n");
		AddLog("        Runs: gh copilot suggest \"<question>\"\n");
		return;
	}

	// Build: gh copilot suggest "<raw_args>" --shell-out
	// We intentionally do NOT interpolate raw_args into a shell glob; wrap in
	// single-quotes and escape any embedded single-quotes (X -> '\''X).
	std::string q(a.raw_args);
	// Escape single-quotes: replace every ' with '\''.
	std::string escaped;
	escaped.reserve(q.size() + 4);
	for (char c : q) {
		if (c == '\'')
			escaped += "'\\''";
		else
			escaped += c;
	}
	std::string cmd = "gh copilot suggest '" + escaped + "'";

	std::shared_ptr<std::atomic<bool>> alive = Alive_;

	// ── Reuse the PTY-backed CmdBash infrastructure
	// ───────────────────────────
	int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (master_fd < 0) {
		AddLog("[error] posix_openpt: %s\n", strerror(errno));
		return;
	}

	if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
		close(master_fd);
		AddLog("[error] grantpt/unlockpt: %s\n", strerror(errno));
		return;
	}

	std::array<char, 256> slave_name{};
	if (ptsname_r(master_fd, slave_name.data(), slave_name.size()) != 0) {
		close(master_fd);
		AddLog("[error] ptsname_r: %s\n", strerror(errno));
		return;
	}

	auto session = std::make_shared<BashSession>();
	session->master_fd.store(master_fd);
	{
		std::lock_guard<std::mutex> lk(BashSessionMutex_);
		ActiveBashSession_ = session;
	}

	AddLog("$ %s\n", cmd.c_str());
	++BashJobCount_;

	pid_t pid = fork();
	if (pid < 0) {
		close(master_fd);
		session->master_fd.store(-1);
		AddLog("[error] fork: %s\n", strerror(errno));
		{
			std::lock_guard<std::mutex> lk(BashSessionMutex_);
			ActiveBashSession_.reset();
		}
		--BashJobCount_;
		return;
	}

	if (pid == 0) {
		setsid();
		int slave_fd = open(slave_name.data(), O_RDWR);
		if (slave_fd < 0)
			_exit(127);
		ioctl(slave_fd, TIOCSCTTY, 0);
		dup2(slave_fd, STDIN_FILENO);
		dup2(slave_fd, STDOUT_FILENO);
		dup2(slave_fd, STDERR_FILENO);
		if (slave_fd > STDERR_FILENO)
			close(slave_fd);
		close(master_fd);
		execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
		_exit(127);
	}

	std::thread worker([this, session, alive, pid]() {
		std::array<char, 512> buf{};
		std::string partial;

		while (true) {
			if (!alive->load())
				break;
			int fd = session->master_fd.load();
			if (fd < 0)
				break;
			struct pollfd pfd{fd, POLLIN, 0};
			int ret = poll(&pfd, 1, 50);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (ret == 0)
				continue;
			if (pfd.revents & (POLLHUP | POLLERR))
				break;
			ssize_t n = read(fd, buf.data(), buf.size() - 1);
			if (n <= 0)
				break;
			buf.at(static_cast<std::size_t>(n)) = '\0';
			for (ssize_t i = 0; i < n; ++i)
				if (buf.at(static_cast<std::size_t>(i)) != '\r')
					partial += buf.at(static_cast<std::size_t>(i));
			size_t pos{};
			while ((pos = partial.find('\n')) != std::string::npos) {
				if (alive->load())
					AddLogThreadSafe(partial.substr(0, pos) + "\n");
				partial.erase(0, pos + 1);
			}
		}

		if (!partial.empty() && alive->load())
			AddLogThreadSafe(partial + "\n");

		int status = 0;
		waitpid(pid, &status, 0);
		if (alive->load()) {
			if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
				AddLogThreadSafe(
					"[error] gh not found — install the GitHub CLI\n");
			else if (WIFEXITED(status) && WEXITSTATUS(status))
				AddLogThreadSafe("[exit " +
								 std::to_string(WEXITSTATUS(status)) + "]\n");
			else if (WIFSIGNALED(status))
				AddLogThreadSafe("[signal " + std::to_string(WTERMSIG(status)) +
								 "]\n");
		}

		{
			std::lock_guard<std::mutex> lk(session->fd_mutex);
			int fd = session->master_fd.exchange(-1);
			if (fd >= 0)
				close(fd);
		}
		session->running.store(false);
		{
			std::lock_guard<std::mutex> lk(BashSessionMutex_);
			if (ActiveBashSession_ == session)
				ActiveBashSession_.reset();
		}
		if (alive->load())
			--BashJobCount_;
	});
	worker.detach();
}

// ── TERMINAL / KONSOLE
// ──────────────────────────────────────────────────────── Strip ANSI/VT100
// escape sequences so shell prompts appear as plain text.

static std::string StripAnsi(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		if (in.at(i) == '\x1b') {
			++i;
			if (i >= in.size())
				break;
			if (in.at(i) == '[') // CSI: ESC [ params final
			{
				++i;
				while (i < in.size() && (in.at(i) < 0x40 || in.at(i) > 0x7e))
					++i;
				if (i < in.size())
					++i;
			} else if (in.at(i) == ']') // OSC: ESC ] ... ST/BEL
			{
				++i;
				while (i < in.size() && in.at(i) != '\x07' && in.at(i) != '\x1b')
					++i;
				if (i < in.size() && in.at(i) == '\x07')
					++i;
				else if (i < in.size() && in.at(i) == '\x1b') {
					++i;
					if (i < in.size())
						++i;
				}
			} else if (in.at(i) == '(' || in.at(i) == ')') // charset designation
			{
				++i;
				if (i < in.size())
					++i;
			} else {
				++i;
			} // two-char escape (ESC M, ESC c …)
		} else if (in.at(i) == '\x07') {
			++i;
		}                           // BEL — discard
		else if (in.at(i) == '\x08') // BS — remove last char
		{
			if (!out.empty())
				out.pop_back();
			++i;
		} else {
			out += in.at(i++);
		}
	}
	return out;
}

// Starts an interactive $SHELL in the PTY and switches the console input bar
// into pass-through mode.  All typed lines are forwarded directly to the shell;
// output (including the prompt) is ANSI-stripped and shown in the log.
// Type 'exit' (or Ctrl+D via the input) to end the session.

void ConsoleCommands::CmdTerminal(const ConsoleCommandArgs & /*a*/) {
	const char *shell = getenv("SHELL");
	if (!shell || !*shell)
		shell = "/bin/bash";

	int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (master_fd < 0) {
		AddLog("[error] posix_openpt: %s\n", strerror(errno));
		return;
	}

	if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
		close(master_fd);
		AddLog("[error] grantpt/unlockpt: %s\n", strerror(errno));
		return;
	}

	std::array<char, 256> slave_name{};
	if (ptsname_r(master_fd, slave_name.data(), slave_name.size()) != 0) {
		close(master_fd);
		AddLog("[error] ptsname_r: %s\n", strerror(errno));
		return;
	}

	// Give the PTY a wide window so bash doesn't hard-wrap at 80 columns.
	struct winsize ws = {};
	ws.ws_col = 220;
	ws.ws_row = 50;
	ioctl(master_fd, TIOCSWINSZ, &ws);

	auto session = std::make_shared<BashSession>();
	session->master_fd.store(master_fd);
	session->terminal_mode.store(true);
	{
		std::lock_guard<std::mutex> lk(BashSessionMutex_);
		ActiveBashSession_ = session;
	}

	AddLog("[tty] Starting %s  (type 'exit' to quit)\n", shell);
	++BashJobCount_;

	pid_t pid = fork();
	if (pid < 0) {
		close(master_fd);
		session->master_fd.store(-1);
		session->terminal_mode.store(false);
		AddLog("[error] fork: %s\n", strerror(errno));
		{
			std::lock_guard<std::mutex> lk(BashSessionMutex_);
			ActiveBashSession_.reset();
		}
		--BashJobCount_;
		return;
	}

	if (pid == 0) {
		// Child: become a session leader, attach PTY slave as controlling tty.
		setsid();
		int slave_fd = open(slave_name.data(), O_RDWR);
		if (slave_fd < 0)
			_exit(127);
		ioctl(slave_fd, TIOCSCTTY, 0);
		dup2(slave_fd, STDIN_FILENO);
		dup2(slave_fd, STDOUT_FILENO);
		dup2(slave_fd, STDERR_FILENO);
		if (slave_fd > STDERR_FILENO)
			close(slave_fd);
		close(master_fd);
		setenv("TERM", "xterm-256color", 1);
		execl(shell, shell, "-i", nullptr);
		_exit(127);
	}

	std::shared_ptr<std::atomic<bool>> alive = Alive_;
	std::thread worker([this, session, alive, pid]() {
		std::array<char, 512> buf{};
		std::string partial;

		while (true) {
			if (!alive->load())
				break;
			int fd = session->master_fd.load();
			if (fd < 0)
				break;

			struct pollfd pfd{fd, POLLIN, 0};
			int ret = poll(&pfd, 1, 50);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (ret == 0)
				continue;
			if (pfd.revents & (POLLHUP | POLLERR))
				break;

			ssize_t n = read(fd, buf.data(), buf.size() - 1);
			if (n <= 0)
				break;
			buf.at(static_cast<std::size_t>(n)) = '\0';

			// Strip \r injected by PTY ONLCR translation.
			for (ssize_t i = 0; i < n; ++i)
				if (buf.at(static_cast<std::size_t>(i)) != '\r')
					partial += buf.at(static_cast<std::size_t>(i));

			// Flush complete lines.
			size_t pos{};
			while ((pos = partial.find('\n')) != std::string::npos) {
				if (alive->load())
					AddLogThreadSafe(StripAnsi(partial.substr(0, pos)) + "\n");
				partial.erase(0, pos + 1);
			}

			// Flush any remaining partial content (e.g. the shell prompt,
			// which has no trailing newline).
			if (!partial.empty() && alive->load()) {
				std::string stripped = StripAnsi(partial);
				if (!stripped.empty())
					AddLogThreadSafe(std::move(stripped));
				partial.clear();
			}
		}

		if (!partial.empty() && alive->load())
			AddLogThreadSafe(StripAnsi(partial) + "\n");

		int status = 0;
		waitpid(pid, &status, 0);
		if (alive->load())
			AddLogThreadSafe("[tty] Shell exited.\n");

		{
			std::lock_guard<std::mutex> lk(session->fd_mutex);
			int fd = session->master_fd.exchange(-1);
			if (fd >= 0)
				close(fd);
		}
		session->running.store(false);
		session->terminal_mode.store(false);
		{
			std::lock_guard<std::mutex> lk(BashSessionMutex_);
			if (ActiveBashSession_ == session)
				ActiveBashSession_.reset();
		}
		if (alive->load())
			--BashJobCount_;
	});
	worker.detach();
}
