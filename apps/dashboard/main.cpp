/*
 * vn-mediabus-dashboard - launcher for the Player and the Controller
 *
 * This is the "mother app": the one thing an operator starts, and the one thing
 * that stays running. It lives in the notification area (the tray) with a menu
 * that starts and stops the other two applications, so the Player and the
 * Controller can be closed and reopened without losing the launcher.
 *
 * Closing its window hides it rather than exiting, and QUIT in the tray menu is
 * the only way out — unless the tray could not be created at all, in which case
 * the window goes back to being an ordinary window that closes normally. That
 * fallback matters: a launcher with no tray and no exit is a process the user
 * cannot get rid of.
 *
 * Liveness is decided by each application's own health endpoint, not by process
 * enumeration: that is portable, it needs no elevation, and it answers the
 * question that matters ("is its API answering?") rather than a proxy for it.
 *
 * Drawing is Dear ImGui on the GL context this file creates: the panel in
 * view/DashboardView never names OpenGL, and never acts - it reports. The tray
 * is shell integration rather than rendering, and the only Win32 detail here,
 * living in TrayIcon.cpp rather than in this file.
 *
 * Copyright (c) vecnode 2026 - GPL-2.0-or-later (see LICENSE)
 */

#include "control/AppLauncher.h"
#include "control/DashboardModel.h"
#include "control/ScriptDocument.h"
#include "control/ScriptLibrary.h"
#include "view/DashboardView.h"
#include "win32/TrayIcon.h"
#include "win32/FolderPicker.h"
#include "gfx/GlLoader.h"
#include "gfx/UiScaleGlfw.h"
#include "net/HttpJsonClient.h"
#include "ui/UiLayer.h"
#include "core/AppConfig.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/UiScale.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
// glfwGetWin32Window lives in the native header, which must come after glfw3.h.
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

GLFWwindow* gWindow = nullptr;

struct Options {
	int width = media::DashboardModel::kDefaultWidth;
	int height = media::DashboardModel::kDefaultHeight;
	/// Start hidden, living only in the tray. What run.bat asks for.
	bool startInTray = false;
	/// Never create the tray icon: an ordinary window that closes normally.
	/// The escape hatch for a machine where Shell_NotifyIcon does not work.
	bool noTray = false;
	/// Which tab to open on. Applications is the useful default; the other two
	/// exist so a script or a screenshot can go straight to what it is checking
	/// without driving the mouse.
	media::DashboardPanel::Tab tab = media::DashboardPanel::Tab::Applications;
};

/// Parse a tab name as --tab accepts it.
bool tabFromName(const std::string& name, media::DashboardPanel::Tab& out) {
	if (name == "applications" || name == "apps") {
		out = media::DashboardPanel::Tab::Applications;
		return true;
	}
	if (name == "corpus" || name == "media" || name == "clips") {
		out = media::DashboardPanel::Tab::Corpus;
		return true;
	}
	if (name == "scripts") {
		out = media::DashboardPanel::Tab::Scripts;
		return true;
	}
	if (name == "activity" || name == "log") {
		out = media::DashboardPanel::Tab::Log;
		return true;
	}
	return false;
}

void printUsage() {
	std::printf(
		"vn-mediabus-dashboard - launcher for the Player and the Controller\n"
		"\n"
		"Usage: vn-mediabus-dashboard.exe [options]\n"
		"\n"
		"  --width N     window width  (default %d pixels)\n"
		"  --height N    window height (default %d pixels)\n"
		"  --tab NAME    open on this tab: applications | corpus | scripts | activity\n"
		"  --tray        start hidden, in the notification area\n"
		"  --no-tray     never add a tray icon; the window is the whole UI and\n"
		"                closing it exits (use this if no tray icon appears)\n"
		"  --help, -h    show this text\n"
		"\n"
		"Tray:  right-click the icon for Launch Player / Launch Controller / Quit.\n"
		"       Left-click shows or hides this window.\n"
		"Window: click LAUNCH on a row. STOP only stops an app this launcher "
		"started.\n"
		"        CHANGE... picks the media corpus folder the Player reads.\n"
		"        The Corpus tab lists the media the Player actually found.\n"
		"Keys:  Esc asks whether to quit. Yes closes THIS launcher only, so the\n"
		"       Player keeps playing; QUIT in the tray menu stops everything.\n",
		media::DashboardModel::kDefaultWidth, media::DashboardModel::kDefaultHeight);
}

bool parseOptions(int argc, char** argv, Options& out) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto nextInt = [&](int& target) {
			if (i + 1 < argc) {
				target = std::atoi(argv[++i]);
			}
		};
		if (arg == "--help" || arg == "-h") {
			printUsage();
			return false;
		} else if (arg == "--width") {
			nextInt(out.width);
		} else if (arg == "--height") {
			nextInt(out.height);
		} else if (arg == "--tray" || arg == "--hidden") {
			out.startInTray = true;
		} else if (arg == "--no-tray") {
			out.noTray = true;
		} else if (arg == "--tab") {
			if (i + 1 < argc) {
				const std::string name = argv[++i];
				if (!tabFromName(name, out.tab)) {
					LOG_WARN("Dashboard") << "unknown tab '" << name
						<< "'; expected applications, scripts or activity";
				}
			}
		} else {
			LOG_WARN("Dashboard") << "ignoring unknown argument: " << arg;
		}
	}
	// --tray asks to be invisible with only an icon to reach it; --no-tray asks
	// for exactly the opposite. Honouring both would produce a process with no
	// icon and no window, so the explicit "no tray" wins and says so.
	if (out.noTray && out.startInTray) {
		LOG_WARN("Dashboard") << "--no-tray overrides --tray: the window is shown";
		out.startInTray = false;
	}
	return true;
}

/// Tell Windows this process understands DPI, before any window exists, so
/// glfwGetMonitorContentScale reports the real scale instead of 1.0 and the
/// text is drawn at real pixels rather than scaled up by the compositor.
/// Mirrors the same call in the Controller and the Player.
void enableDpiAwareness() {
#if defined(_WIN32)
	typedef BOOL (WINAPI *SetAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
	if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
		auto setContext = reinterpret_cast<SetAwarenessContextFn>(
			reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
		if (setContext != nullptr
			&& setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
			return;
		}
	}
	SetProcessDPIAware();
#endif
}

/// Ask for a window whose FRAMEBUFFER is `wantW` x `wantH` physical pixels.
///
/// Everything that draws works in framebuffer pixels: ImGui's DisplaySize, the GL
/// viewport, and the layout itself. glfwCreateWindow takes SCREEN COORDINATES,
/// and on a scaled display the framebuffer that comes back is that size times the
/// content scale - so the sizes have to be reconciled, and the result verified
/// rather than assumed. See the identical note in apps/controller/main.cpp.
bool syncWindowToFramebuffer(GLFWwindow* window, int wantW, int wantH) {
	int fbW = 0;
	int fbH = 0;
	glfwGetFramebufferSize(window, &fbW, &fbH);
	if (fbW == wantW && fbH == wantH) {
		return true;
	}
	int winW = 0;
	int winH = 0;
	glfwGetWindowSize(window, &winW, &winH);
	if (fbW <= 0 || fbH <= 0 || winW <= 0 || winH <= 0) {
		LOG_WARN("Dashboard") << "no usable window or framebuffer size; "
			"laying out for " << fbW << "x" << fbH;
		return false;
	}
	const int targetW = std::max(1, static_cast<int>(
		static_cast<float>(winW) * (static_cast<float>(wantW) / static_cast<float>(fbW)) + 0.5f));
	const int targetH = std::max(1, static_cast<int>(
		static_cast<float>(winH) * (static_cast<float>(wantH) / static_cast<float>(fbH)) + 0.5f));
	glfwSetWindowSize(window, targetW, targetH);
	glfwGetFramebufferSize(window, &fbW, &fbH);
	if (fbW == wantW && fbH == wantH) {
		return true;
	}
	LOG_WARN("Dashboard") << "framebuffer is " << fbW << "x" << fbH
		<< ", not the requested " << wantW << "x" << wantH
		<< "; the launcher will be laid out for the size it really has";
	return false;
}

void onGlfwError(int code, const char* description) {
	LOG_ERROR("GLFW") << code << ": " << description;
}

/// The launcher state: one probe per application plus the model the view draws.
///
/// Probing does a real HTTP request, so it runs on its own thread and publishes
/// a snapshot under a mutex — never on the frame loop, which must not wait on a
/// socket even with a short timeout.
class Dashboard {
public:
	/// What the Player says about its corpus, read fresh on every probe pass.
	///
	/// The Player is the authority here: it owns the library, so asking it is
	/// what keeps this panel from reporting a folder the decoder is not using.
	struct CorpusSnapshot {
		bool online = false;
		/// The folders the Player reported it is reading. A list, because the
		/// corpus is the merge of all of them, and a panel that shows only the
		/// first cannot explain where a clip came from.
		std::vector<std::string> folders;
		std::size_t clipCount = 0;
	};

	Dashboard()
		: player_(media::DashboardApp::Player),
		  controller_(media::DashboardApp::Controller) {}

	void start() {
		refresh();
		thread_ = std::thread([this] {
			while (!stopping_.load()) {
				// Cheap enough to poll often, and it is what makes a launch
				// from another shell show up here within a fraction of a second.
				for (int slept = 0; slept < 700 && !stopping_.load(); slept += 20) {
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
				if (!stopping_.load()) {
					refresh();
				}
			}
		});
	}

	void stop() {
		stopping_ = true;
		if (thread_.joinable()) {
			thread_.join();
		}
	}

	/// One synchronous pass: probe both applications, read the Player's corpus
	/// state, and publish the result.
	void refresh() {
		player_.probe();
		controller_.probe();
		CorpusSnapshot corpus = readCorpus();

		std::lock_guard<std::mutex> lock(mutex_);
		playerStatus_ = player_.status();
		controllerStatus_ = controller_.status();
		playerPath_ = player_.executablePath();
		controllerPath_ = controller_.executablePath();
		corpus_ = std::move(corpus);
	}

	/// Publish the latest snapshot into the model the view draws.
	void applyTo(media::DashboardModel& model) {
		std::lock_guard<std::mutex> lock(mutex_);
		model.setStatus(media::DashboardApp::Player, playerStatus_, playerPath_,
			media::AppProbe::kPlayerPort);
		model.setStatus(media::DashboardApp::Controller, controllerStatus_,
			controllerPath_, media::AppProbe::kControllerPort);

		// The *configured* folders are what this Dashboard is offering; the live
		// count comes from the Player. Keeping both means the panel stays honest
		// when someone edits mediabus.ini or starts the Player with a different
		// set than the one recorded here.
		media::config::Config config;
		media::config::load(config);
		model.setCorpus(config.mediaFolders, corpus_.clipCount, corpus_.online,
			corpus_.folders);
	}

	void handle(media::DashboardAction action, media::DashboardModel& model,
		const std::string& folder = std::string()) {
		std::string error;
		bool ok = false;
		switch (action) {
			case media::DashboardAction::LaunchPlayer:
				ok = player_.launch(error);
				break;
			case media::DashboardAction::LaunchController:
				ok = controller_.launch(error);
				break;
			case media::DashboardAction::StopPlayer:
				ok = player_.stop(error);
				break;
			case media::DashboardAction::StopController:
				ok = controller_.stop(error);
				break;
			case media::DashboardAction::ChooseMediaFolder:
				addMediaFolder(model);
				return;
			case media::DashboardAction::RemoveMediaFolder:
				// The folder travels in the frame, not in the action: with several
				// rows on screen the action alone cannot say which was clicked.
				removeMediaFolder(model, folder);
				return;
			case media::DashboardAction::None:
				return;
		}
		model.setMessage(ok ? std::string("ok") : error);
		refresh();
	}

	/// Ask for a folder, persist it, and apply it to a running Player.
	///
	/// Runs on the main thread: the picker is a modal dialog with its own
	/// message loop, so it must never run on the probe thread or on an HTTP
	/// worker.
	/// Ask for a folder to ADD to the corpus, then apply the longer list.
	///
	/// Runs on the main thread: the picker is a modal dialog with its own
	/// message loop, so it must never run on the probe thread or on an HTTP
	/// worker.
	void addMediaFolder(media::DashboardModel& model) {
		bool cancelled = false;
		media::config::Config current;
		media::config::load(current);
		const std::string chosen = media::ui::pickFolder(
			"Add a media folder to the corpus", current.primaryMediaFolder(),
			&cancelled);
		if (cancelled) {
			return;   // a cancelled dialog is not an outcome worth reporting
		}
		if (chosen.empty()) {
			model.setMessage(media::ui::folderPickerAvailable()
				? "no folder chosen" : "no folder picker in this build");
			return;
		}

		// APPENDED, not replaced: the corpus is the merge of every folder listed,
		// which is the entire point of allowing more than one. A folder that is
		// already there is reported rather than added twice.
		std::vector<std::string> wanted = current.mediaFolders;
		bool alreadyThere = false;
		for (const std::string& folder : wanted) {
			if (folder == chosen) {
				alreadyThere = true;
				break;
			}
		}
		if (!alreadyThere) {
			wanted.push_back(chosen);
		}

		applyFolders(model, wanted, alreadyThere
			? "already in the corpus: " + chosen
			: "added to the corpus: " + chosen);
	}

	/// Drop one folder from the corpus and apply the shorter list.
	void removeMediaFolder(media::DashboardModel& model, const std::string& folder) {
		if (folder.empty()) {
			return;
		}
		media::config::Config current;
		media::config::load(current);

		std::vector<std::string> wanted;
		wanted.reserve(current.mediaFolders.size());
		for (const std::string& entry : current.mediaFolders) {
			if (entry != folder) {
				wanted.push_back(entry);
			}
		}
		if (wanted.size() == current.mediaFolders.size()) {
			model.setMessage("not in the corpus: " + folder);
			return;
		}

		applyFolders(model, wanted, "removed from the corpus: " + folder);
	}

	/// Persist `folders`, hand them to a running Player, and report what happened.
	///
	/// One place for both edits, so add and remove cannot disagree about how the
	/// setting is written or about what an absent Player is told.
	void applyFolders(media::DashboardModel& model,
		const std::vector<std::string>& folders, const std::string& note) {
		std::string message = note;

		if (player_.status().running()) {
			// While the Player is up it is the SINGLE WRITER of this setting: it
			// persists the new list itself, in the same call. Writing the file
			// here as well would be exactly the two-writers race that lets the
			// displayed folders and the scanned folders drift apart.
			media::HttpJsonClient client("127.0.0.1", media::AppProbe::kPlayerPort);
			const media::HttpJsonClient::Json body{{"paths", folders}};
			media::HttpJsonClient::Json reply;
			std::string postError;
			if (!client.post("/api/media-dir", body, reply, postError)) {
				message += "  [warning] the Player refused it: " + postError;
			}
		} else {
			// Nobody is up, so this is the fallback writer the design allows.
			media::config::Config current;
			media::config::load(current);
			current.mediaFolders = folders;
			if (!media::config::save(current)) {
				message += "  [warning] could not write " + media::config::configPath();
			}
			message += "  (takes effect when the Player starts)";
		}

		model.setMessage(message);
		refresh();
	}

private:
	/// One GET /api/media-dir. Cheap (localhost, small body) and short-fused,
	/// so it is safe on the probe thread. A Player that is down just leaves the
	/// snapshot offline, which is a state the panel draws.
	CorpusSnapshot readCorpus() const {
		CorpusSnapshot snapshot;
		if (!player_.status().running()) {
			return snapshot;
		}
		media::HttpJsonClient client("127.0.0.1", media::AppProbe::kPlayerPort);
		media::HttpJsonClient::Json reply;
		std::string error;
		if (!client.get("/api/media-dir", reply, error) || !reply.is_object()) {
			return snapshot;
		}
		snapshot.online = true;
		if (reply.contains("mediaFolders") && reply["mediaFolders"].is_array()) {
			for (const auto& entry : reply["mediaFolders"]) {
				if (entry.is_string()) {
					snapshot.folders.push_back(entry.get<std::string>());
				}
			}
		} else if (reply.contains("mediaFolder") && reply["mediaFolder"].is_string()) {
			// An older Player answers with the singular field only. Reading it
			// here rather than refusing keeps the launcher usable against a build
			// that predates the list.
			const std::string one = reply["mediaFolder"].get<std::string>();
			if (!one.empty()) {
				snapshot.folders.push_back(one);
			}
		}
		if (reply.contains("clipCount") && reply["clipCount"].is_number_unsigned()) {
			snapshot.clipCount = reply["clipCount"].get<std::size_t>();
		}
		return snapshot;
	}

	media::AppProbe player_;
	media::AppProbe controller_;
	std::thread thread_;
	std::atomic<bool> stopping_{false};

	std::mutex mutex_;
	media::AppStatus playerStatus_;
	media::AppStatus controllerStatus_;
	std::string playerPath_;
	std::string controllerPath_;
	CorpusSnapshot corpus_;
};

/// Everything the window callbacks need. One instance lives for the whole run,
/// and the window's user-data slot points at it.
///
/// The interface itself is ImGui's now, so there is no click state here: ImGui
/// owns the pointer and the keyboard inside the window, and what the operator
/// did arrives as a DashboardAction. What is left is the tray's business, which
/// ImGui knows nothing about.
struct UiState {
	/// True once the tray icon exists. While it does, closing or hiding the
	/// window keeps the launcher alive; without it, the window is the only way
	/// to quit and must behave normally.
	bool trayAvailable = false;
	/// Set by QUIT in the tray menu, or by Yes in the Esc confirmation: the way
	/// out.
	bool quit = false;
	/// Whether leaving should take the applications this launcher started with
	/// it.
	///
	/// True for QUIT in the tray menu, which is the explicit "I am done" and
	/// says so. FALSE for the Esc-confirmed quit, which must leave the Player
	/// and the Controller alone: Esc is also the key that pulls the Player out of
	/// fullscreen, so an Esc that stopped playback would be the exact thing this
	/// change exists to prevent.
	bool stopChildrenOnExit = true;
};

void setWindowVisible(GLFWwindow* window, bool visible) {
	if (visible) {
		glfwShowWindow(window);
		glfwFocusWindow(window);
	} else {
		glfwHideWindow(window);
	}
}

/// Hover text for the tray icon: the one thing visible while the window is not.
std::string trayTooltip(const media::DashboardModel& model) {
	std::string text = "vn-mediabus launcher";
	for (const media::DashboardRow& row : model.rows()) {
		text += row.app == media::DashboardApp::Player ? " | Player: " : " | Controller: ";
		text += row.running ? "up" : "down";
	}
	return text;
}

/// An empty script that is valid Lua and says what to do with it.
///
/// Starting a new file from nothing gives no clue about the API a script is
/// written against, and `controller` is not something a person guesses. The
/// header comment is the cheapest possible documentation.
std::string newScriptTemplate(const std::string& name) {
	return "-- " + name + "\n"
		"--\n"
		"-- Runs inside the Controller. Every call below is an HTTP request to the\n"
		"-- Player's control API, so a script cannot break the Player.\n"
		"--\n"
		"-- OnTick is called once per frame. controller.Sleep(ms) does not block the\n"
		"-- window: it charges a per-frame budget and resumes on a later frame, which\n"
		"-- is what lets a sequence of steps read as a sequence.\n"
		"--\n"
		"-- Available: Play, Next, Previous, Pause, Stop, SeekPercent, SetVolume,\n"
		"-- SetSpeed, SetSubtitles, ShowHUD, Fullscreen, CurrentClip, Playlist, Log,\n"
		"-- Sleep, OnTick.\n"
		"\n"
		"local M = {}\n"
		"\n"
		"function M.onTick()\n"
		"\tlocal clip = controller.CurrentClip()\n"
		"\tif not clip.online then\n"
		"\t\treturn\n"
		"\tend\n"
		"\tcontroller.Log(\"running: \" .. tostring(clip.name))\n"
		"\treturn\n"
		"end\n"
		"\n"
		"controller.OnTick(M.onTick)\n";
}

/// Load one script's text into the editor.
///
/// The document has to be told WHICH script it holds as well as what is in it:
/// setText() alone leaves `loaded()` false, because a buffer with no name has
/// nowhere to save to. Getting that wrong is what made the editor keep saying
/// "select a script" while a script was plainly selected - so this goes through
/// create(), which adopts the name under the same containment rule load() uses,
/// and then sets the text.
///
/// Returns false and leaves the document alone when the Controller cannot supply
/// the text, so a failure cannot silently blank what the operator was editing.
bool openScriptIntoEditor(const std::string& name, media::DashboardPanel& panel,
	media::ScriptLibrary& library, media::ScriptDocument& document) {
	std::string text;
	const media::ScriptLibrary::Result read = library.read(name, text);
	if (!read.ok) {
		panel.setScriptStatus("controller: " + read.error);
		return false;
	}

	std::string error;
	if (!document.create(name, media::kControllerScriptsSubdirectory, error)) {
		panel.setScriptStatus(error);
		return false;
	}
	document.setText(std::move(text));
	// Freshly read from disk is not a local edit.
	document.markSaved();
	document.clearError();
	panel.setSelection(name);
	panel.setDirty(false);
	return true;
}

/// Act on everything the Scripts tab asked for this frame.
///
/// Kept as a free function rather than a method on the panel so the panel stays
/// a drawing class with no network and no file access - which is the property
/// that lets it be reasoned about, and the reason the Scripts tab can be tested
/// by driving ScriptLibrary directly.
void handleScriptRequests(media::DashboardPanel& panel, media::ScriptLibrary& library,
	media::ScriptDocument& document, media::DashboardModel& model) {
	if (!panel.saveRequested() && !panel.validateRequested()
		&& !panel.reloadRequested() && !panel.runRequested()
		&& panel.newScriptName().empty()) {
		return;
	}

	const std::string newName = panel.newScriptName();
	const std::string selection = panel.selection();
	panel.clearRequests();

	// --- create ------------------------------------------------------------
	if (!newName.empty()) {
		std::string name = newName;
		if (media::platform::lowerExtension(name) != ".lua") {
			name += ".lua";
		}
		const media::ScriptLibrary::Result written = library.write(name,
			newScriptTemplate(name));
		panel.setScriptStatus(written.ok ? "created " + name : written.error);
		if (written.ok) {
			// Same path as opening an existing one, so the new script's name is
			// adopted too - setText() alone leaves a document with nowhere to save.
			openScriptIntoEditor(name, panel, library, document);
			model.log("created script " + name);
		}
		return;
	}

	if (selection.empty()) {
		panel.setScriptStatus("select a script first");
		return;
	}

	// --- validate ----------------------------------------------------------
	if (panel.validateRequested()) {
		media::ScriptValidation validation;
		const media::ScriptLibrary::Result result = library.validate(selection,
			document.text(), validation);
		if (!result.ok) {
			panel.setScriptStatus("controller: " + result.error);
		} else if (validation.ok) {
			document.clearError();
			panel.setScriptStatus("compiles: no syntax errors");
		} else {
			document.setErrorLine(validation.errorLine, validation.message);
			panel.revealError(validation.errorLine);
			panel.setScriptStatus("does not compile");
		}
		return;
	}

	// --- save --------------------------------------------------------------
	if (panel.saveRequested()) {
		const media::ScriptLibrary::Result result = library.write(selection,
			document.text());
		if (!result.ok) {
			panel.setScriptStatus(result.error);
			// A refusal that names a line is a syntax error: mark it, so the
			// editor shows where rather than only that.
			return;
		}
		document.clearError();
		document.markSaved();
		panel.setDirty(false);
		panel.setScriptStatus("saved " + selection);
		model.log("saved script " + selection);

		// Saving the script that is running and then not reloading it would leave
		// the operator looking at code the Controller is not executing. Reloading
		// is a separate request so it stays visible in the log when it happens.
		const media::ScriptLibrary::Result reloaded = library.reload();
		if (reloaded.ok) {
			panel.setScriptStatus("saved and reloaded " + selection);
			model.log("reloaded " + selection + " into the Controller");
		} else {
			panel.setScriptStatus("saved " + selection
				+ " - the Controller did not reload it: " + reloaded.error);
		}
		return;
	}

	// --- reload ------------------------------------------------------------
	if (panel.reloadRequested()) {
		const media::ScriptLibrary::Result result = library.reload();
		panel.setScriptStatus(result.ok ? "reloaded " + selection : result.error);
		if (result.ok) {
			model.log("reloaded " + selection + " into the Controller");
		}
		return;
	}

	// --- run ---------------------------------------------------------------
	if (panel.runRequested()) {
		const media::ScriptLibrary::Result result = library.run(selection);
		panel.setScriptStatus(result.ok ? "running " + selection : result.error);
		if (result.ok) {
			model.log("started script " + selection);
		}
	}
}

/// Read the Player's playlist for the Corpus tab.
///
/// The launcher is an HTTP client of the Player, exactly as it is of the
/// Controller, so the list comes from the one process that owns the library
/// rather than from a second, possibly disagreeing reading of the folder.
///
/// On failure the list is CLEARED rather than left standing: a stale list next
/// to a Player that is down would read as "this is what you have", when the
/// truth is that nobody could be asked. The status line says which.
void refreshCorpus(media::DashboardModel& model) {
	media::HttpJsonClient client("127.0.0.1", media::AppProbe::kPlayerPort);
	media::HttpJsonClient::Json reply;
	std::string error;
	if (!client.get("/api/clips", reply, error)) {
		model.setClipList({});
		model.setCorpusStatus("player: " + error);
		return;
	}
	if (!reply.is_array()) {
		model.setClipList({});
		model.setCorpusStatus("player: /api/clips did not answer with a list");
		return;
	}

	std::vector<media::CorpusEntry> clips;
	clips.reserve(reply.size());
	for (const auto& item : reply) {
		media::CorpusEntry entry;
		if (item.contains("index") && item["index"].is_number_unsigned()) {
			entry.index = item["index"].get<std::size_t>();
		}
		if (item.contains("name") && item["name"].is_string()) {
			entry.name = item["name"].get<std::string>();
		}
		if (item.contains("path") && item["path"].is_string()) {
			entry.path = item["path"].get<std::string>();
		}
		if (item.contains("mediaType") && item["mediaType"].is_string()) {
			entry.mediaType = item["mediaType"].get<std::string>();
		}
		clips.push_back(std::move(entry));
	}

	model.setClipList(std::move(clips));
	// The panel draws the count itself, so the status carries only what the list
	// cannot say - which here is nothing.
	model.setCorpusStatus(std::string());
}

} // namespace

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	media::log::setThresholdFromEnv();
	// Windows-subsystem binary: with no console to write to, the log goes to
	// bin/mediabus-dashboard.log. See core/Log.cpp.
	media::log::useFileSink("dashboard");

	Options options;
	if (!parseOptions(argc, argv, options)) {
		return 0;
	}

	enableDpiAwareness();

	glfwSetErrorCallback(onGlfwError);
	if (glfwInit() != GLFW_TRUE) {
		LOG_ERROR("Dashboard") << "glfwInit failed";
		return 1;
	}

	// Text size, by the same DPI rule as the other two windows (core/UiScale.h).
	// Read before the window exists so the initial layout is already right.
	media::config::Config config;
	media::config::load(config);
	const float contentScale = media::ui::rawContentScale();
	const float uiScale = media::ui::scaleForWindow(config);

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	glfwWindowHint(GLFW_SAMPLES, 0);
	// --tray means the icon is the whole user interface until it is asked for.
	glfwWindowHint(GLFW_VISIBLE, options.startInTray ? GLFW_FALSE : GLFW_TRUE);

	// The window is asked for in SCREEN COORDINATES, and the framebuffer it
	// produces is that size times the monitor's content scale - on a 150%
	// display, asking for 1040 gives a 1560-pixel framebuffer. The layout and the
	// fonts are both measured in FRAMEBUFFER pixels, so asking for the layout size
	// directly made everything one content-scale larger than intended. The
	// division below removes that, and syncWindowToFramebuffer afterwards
	// corrects whatever the display actually did.
	int wantW = std::max(1, static_cast<int>(
		static_cast<float>(options.width) / uiScale + 0.5f));
	int wantH = std::max(1, static_cast<int>(
		static_cast<float>(options.height) / uiScale + 0.5f));
	gWindow = glfwCreateWindow(wantW, wantH, "vn-mediabus-dashboard", nullptr, nullptr);
	if (gWindow == nullptr) {
		LOG_ERROR("Dashboard") << "glfwCreateWindow failed";
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(gWindow);
	glfwSwapInterval(1);

	// GLEW through the shared helper: correct ordering, and the drain that
	// clears the spurious core-profile GL_INVALID_ENUM glewInit leaves in the
	// error queue. See libs/gfx/GlLoader.h.
	if (!media::gfx::initializeGlLoader(gWindow)) {
		LOG_ERROR("Dashboard") << "glewInit failed";
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}

	// The RenderDevice this application used to draw through is gone: every card,
	// button, label and bitmap glyph it drew is an ImGui widget now, and ImGui
	// owns the GL objects behind them. Nothing here needs the device, which is
	// why this file no longer creates one.

	media::DashboardModel model;
	Dashboard dashboard;

	dashboard.refresh();
	dashboard.applyTo(model);

	// --- input and tray ---------------------------------------------------
	UiState ui;
	glfwSetWindowUserPointer(gWindow, &ui);

	// The tray menu runs the same two things the window's buttons do, through
	// the same Dashboard object, so there is no second control path that could
	// disagree with the window about what is running.
	media::TrayIcon tray;
	const auto onTray = [&](media::TrayAction action) {
		switch (action) {
			case media::TrayAction::LaunchPlayer:
				dashboard.handle(media::DashboardAction::LaunchPlayer, model);
				break;
			case media::TrayAction::LaunchController:
				dashboard.handle(media::DashboardAction::LaunchController, model);
				break;
			case media::TrayAction::StopPlayer:
				dashboard.handle(media::DashboardAction::StopPlayer, model);
				break;
			case media::TrayAction::StopController:
				dashboard.handle(media::DashboardAction::StopController, model);
				break;
			case media::TrayAction::ToggleWindow:
				setWindowVisible(gWindow, glfwGetWindowAttrib(gWindow, GLFW_VISIBLE) == 0);
				break;
			case media::TrayAction::Quit:
				ui.quit = true;
				break;
			case media::TrayAction::None:
				break;
		}
	};

	// --no-tray: skip the icon entirely and be an ordinary window. This is the
	// escape hatch for a machine where Shell_NotifyIcon is refused, which is
	// not something this process can detect any other way than by being asked.
	if (options.noTray) {
		LOG_NOTICE("Dashboard") << "--no-tray: no icon; the window is the whole UI "
			"and closing it exits";
	} else {
		switch (tray.create(trayTooltip(model), onTray)) {
			case media::TrayResult::Created:
				ui.trayAvailable = true;
				break;

			case media::TrayResult::AlreadyRunning:
				// A launcher is already in the tray for this session. Exit
				// quietly: becoming a second window would be a worse answer
				// than doing nothing, since the one already there can do
				// everything this one could.
				LOG_NOTICE("Dashboard") << "the launcher is already running; nothing to do";
				dashboard.stop();
							glfwDestroyWindow(gWindow);
				glfwTerminate();
				return 0;

			case media::TrayResult::Unavailable:
			case media::TrayResult::Unsupported:
				// Never leave an invisible process behind, and never leave one
				// whose only exit is a tray icon that does not exist. The window
				// is shown whatever was asked for, and the status line says why,
				// because "the launcher did not start" is what a hidden window
				// with no icon looks like from the outside.
				LOG_WARN("Dashboard") << "no tray icon: running as an ordinary window, "
					"where closing really exits";
				glfwShowWindow(gWindow);
				model.setMessage("no tray icon available here - this window is now "
					"the launcher; closing it exits");
				break;
		}
	}

	glfwSetWindowCloseCallback(gWindow, [](GLFWwindow* window) {
		auto* state = static_cast<UiState*>(glfwGetWindowUserPointer(window));
		if (state == nullptr || !state->trayAvailable) {
			return;   // no tray: closing really closes, or there is no way out
		}
		glfwSetWindowShouldClose(window, GLFW_FALSE);
		glfwHideWindow(window);
	});

	// --- the interface ------------------------------------------------------
	// ImGui owns the window's input from here on, which is why the hand-rolled
	// mouse and key callbacks above are gone. The tray is unaffected: it is not a
	// window, so ImGui has no opinion about the icon.
	std::unique_ptr<media::ui::UiLayer> uiLayer = media::ui::UiLayer::create(gWindow);
	if (!uiLayer || !uiLayer->ready()) {
		LOG_ERROR("Dashboard") << "ImGui failed to start; the launcher cannot draw";
		dashboard.stop();
			glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}
	uiLayer->setUiScale(uiScale);
	uiLayer->setIniPath(media::platform::executableDirectory() + "mediabus-ui.ini");
	// Correct the framebuffer to the size the layout was written for. See the
	// identical call in apps/controller/main.cpp.
	syncWindowToFramebuffer(gWindow, options.width, options.height);
	// ImGui chains whatever callbacks were already registered, and the window
	// close callback above must keep working, so it goes first.
	uiLayer->installCallbacks();

	media::DashboardPanel panel;
	panel.setTab(options.tab);
	// The Dashboard is a client of the Controller's script API, exactly as the
	// Controller is a client of the Player's: it never touches the scripts
	// directory itself. See ScriptLibrary.
	media::ScriptLibrary scripts("127.0.0.1", media::AppProbe::kControllerPort);
	media::ScriptDocument document;

	int fbW = 0;
	int fbH = 0;
	glfwGetFramebufferSize(gWindow, &fbW, &fbH);
	dashboard.start();

	LOG_NOTICE("Dashboard") << "vn-mediabus-dashboard";
	LOG_NOTICE("Dashboard") << "  interface      : " << uiLayer->describe();
	LOG_NOTICE("Dashboard") << "  fonts          : " << uiLayer->fonts().uiSource
		<< " / " << uiLayer->fonts().monoSource;
	LOG_NOTICE("Dashboard") << "  text scale     : "
		<< media::ui::describeDecision(uiScale, contentScale);
	LOG_NOTICE("Dashboard") << "  config file    : " << media::config::configPath();
	LOG_NOTICE("Dashboard") << "  folder picker  : "
		<< (media::ui::folderPickerAvailable() ? "available" : "NOT AVAILABLE in this build");
	LOG_NOTICE("Dashboard") << "  player API     : 127.0.0.1:"
		<< media::AppProbe::kPlayerPort;
	LOG_NOTICE("Dashboard") << "  controller API : 127.0.0.1:"
		<< media::AppProbe::kControllerPort;
	LOG_NOTICE("Dashboard") << "  exit           : QUIT in the tray menu"
		<< (ui.trayAvailable ? "" : " (no tray; close the window instead)");

	// When the script list was last refreshed from the Controller, so the panel
	// does not ask on every frame.
	std::chrono::steady_clock::time_point lastScriptRefresh{};
	// When the playlist was last read from the Player, for the Corpus tab.
	std::chrono::steady_clock::time_point lastCorpusRefresh{};
	std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
	bool wasVisible = true;
	// Which tab the panel actually drew last frame. Starts as the requested one,
	// because that is what the first draw will open.
	media::DashboardPanel::Tab visibleTab = options.tab;

	while (!ui.quit && glfwWindowShouldClose(gWindow) == GLFW_FALSE) {
		glfwPollEvents();
		if (ui.quit) {
			break;
		}

		dashboard.applyTo(model);

		// Keep the menu and the hover text honest about what is running, and
		// about which applications this launcher is allowed to stop.
		media::TrayState trayState;
		for (const media::DashboardRow& row : model.rows()) {
			const bool isPlayer = row.app == media::DashboardApp::Player;
			if (isPlayer) {
				trayState.playerRunning = row.running;
				// Same rule the window's STOP button uses: only a running child
				// of this process may be stopped from here.
				trayState.playerManaged = row.running && row.managed;
			} else {
				trayState.controllerRunning = row.running;
				trayState.controllerManaged = row.running && row.managed;
			}
		}
		const bool windowVisible = glfwGetWindowAttrib(gWindow, GLFW_VISIBLE) != 0;
		trayState.windowVisible = windowVisible;
		tray.setState(trayState);
		tray.setTooltip(trayTooltip(model));

		if (!windowVisible && ui.trayAvailable) {
			// Hidden in the tray. Block until something happens - a tray click
			// wakes this, because GLFW waits on every message for the thread, not
			// only on its own window's - rather than spinning a frame loop nobody
			// can see.
			//
			// Note that NO ImGui frame is produced here, on purpose: a hidden
			// window has nothing to draw, and beginFrame without endFrame would
			// leave the draw data from the last visible frame waiting.
			wasVisible = false;
			glfwWaitEventsTimeout(0.5);
			continue;
		}
		if (!wasVisible) {
			// Coming back from the tray: the delta clock was stopped for as long
			// as the window was hidden, and ImGui divides by DeltaTime.
			wasVisible = true;
			lastFrame = std::chrono::steady_clock::now();
		}

		// --- the script list, refreshed a few times a second ----------------
		// A localhost GET, so it is cheap, but not so cheap that it belongs in a
		// 60 Hz loop. The Controller is the only thing that can answer it.
		//
		// `visibleTab` is what the panel actually drew last frame, not what it was
		// asked to open on: Dear ImGui owns the tab bar, so once the operator
		// clicks another tab only the drawn frame knows.
		if (visibleTab == media::DashboardPanel::Tab::Scripts) {
			const auto now = std::chrono::steady_clock::now();
			if (now - lastScriptRefresh > std::chrono::seconds(2)) {
				lastScriptRefresh = now;
				std::vector<media::ScriptEntry> entries;
				const media::ScriptLibrary::Result listed = scripts.list(entries);
				if (listed.ok) {
					// Mark which one is running, from the Controller's own answer.
					for (media::ScriptEntry& entry : entries) {
						entry.open = (entry.name == document.name());
					}
					// Select the first script when the operator has not chosen one.
					// Without this the editor sits empty next to a full list, and
					// the list looks broken rather than waiting.
					if (panel.selection().empty() && !entries.empty()) {
						openScriptIntoEditor(entries.front().name, panel, scripts,
							document);
					}
					panel.setScriptList(std::move(entries));
				} else {
					panel.setScriptStatus("controller: " + listed.error);
				}
			}
		}

		// --- the playlist, for the Corpus tab -------------------------------
		// Same reasoning as the script list above, and the same timer: a
		// localhost GET is cheap but not free, and /api/clips returns the WHOLE
		// playlist - on a large corpus that is a real body. Asking for it while
		// the operator is looking at another tab would be work nobody sees, so
		// this only runs while the Corpus tab is the one actually drawn.
		if (visibleTab == media::DashboardPanel::Tab::Corpus) {
			const auto now = std::chrono::steady_clock::now();
			if (now - lastCorpusRefresh > std::chrono::seconds(2)) {
				lastCorpusRefresh = now;
				refreshCorpus(model);
			}
		}

		glfwGetFramebufferSize(gWindow, &fbW, &fbH);
		if (fbW <= 0 || fbH <= 0) {
			glfwWaitEventsTimeout(0.05);
			continue;
		}

		const auto frameNow = std::chrono::steady_clock::now();
		const double dt = std::chrono::duration<double>(frameNow - lastFrame).count();
		lastFrame = frameNow;

		uiLayer->beginFrame({fbW, fbH, uiScale, dt});

		media::DashboardPanel::Frame frame = panel.draw(*uiLayer, model, &scripts,
			document, model.rowFor(media::DashboardApp::Controller).running);
		visibleTab = frame.tab;

		if (frame.requestQuit) {
			// Yes to the Esc confirmation. It quits the LAUNCHER and nothing
			// else: the Player and the Controller keep running and keep answering
			// their APIs, so a fullscreen Player is not interrupted and the
			// session does not die to a keystroke. QUIT in the tray menu is the
			// shutdown that takes them with it.
			ui.stopChildrenOnExit = false;
			ui.quit = true;
		}
		if (frame.action != media::DashboardAction::None) {
			// actionFolder carries the folder a RemoveMediaFolder refers to; it is
			// empty for every other action.
			dashboard.handle(frame.action, model, frame.actionFolder);
		}
		handleScriptRequests(panel, scripts, document, model);

		// The operator picked a different script in the list. The panel reports the
		// selection; loading the text is this loop's job, because the panel is not
		// allowed to touch the network.
		//
		// Done after the draw so the click that caused it is the one being acted
		// on, and guarded on the document actually disagreeing - otherwise every
		// frame would re-read the file and throw away unsaved edits.
		if (!panel.selection().empty() && panel.selection() != document.name()) {
			openScriptIntoEditor(panel.selection(), panel, scripts, document);
		}

		uiLayer->endFrame();
		glfwSwapBuffers(gWindow);
	}

	if (ui.stopChildrenOnExit) {
		// QUIT in the tray menu is an explicit "I am done", so it takes the
		// applications this launcher started with it. One that someone else
		// started is left alone: AppProbe::stop refuses anything that is not this
		// process's child.
		LOG_NOTICE("Dashboard") << "quitting; stopping the applications this launcher started";
		dashboard.handle(media::DashboardAction::StopPlayer, model);
		dashboard.handle(media::DashboardAction::StopController, model);
	} else {
		LOG_NOTICE("Dashboard") << "quitting the launcher only; the Player and the "
			"Controller keep running";
	}

	dashboard.stop();
	tray.destroy();
	glfwDestroyWindow(gWindow);
	glfwTerminate();
	return 0;
}
