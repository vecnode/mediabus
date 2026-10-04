#pragma once

#include <functional>
#include <memory>
#include <string>

namespace media {

/// An item of the launcher's tray menu.
enum class TrayAction {
	None,
	LaunchPlayer,
	LaunchController,
	StopPlayer,
	StopController,
	ToggleWindow,
	Quit,
};

/// What the popup menu should offer right now.
///
/// Pushed by the frame loop, which is the only place that knows what the probes
/// last saw and whether the window is showing.
struct TrayState {
	bool windowVisible = true;
	bool playerRunning = false;
	bool controllerRunning = false;
	/// True when this launcher started the application, which is the only case
	/// in which STOP is allowed to act on it.
	bool playerManaged = false;
	bool controllerManaged = false;
};

/// How `TrayIcon::create` ended. The three failure modes need different answers,
/// which is why this is not a bool:
///   - AlreadyRunning: this session already has a launcher in the tray, so the
///     new process should exit rather than become a second launcher window.
///   - Unavailable: the shell refused the icon (a locked or remote session can
///     do that), so this process must stay reachable as an ordinary window.
///   - Unsupported: no notification area on this platform at all.
enum class TrayResult {
	Created,
	AlreadyRunning,
	Unavailable,
	Unsupported,
};

/// The launcher's home in the notification area — the Windows tray.
///
/// A hidden message window owns the icon and the popup menu. Nothing here draws
/// and nothing here names a graphics API, so it stays clear of the RenderDevice
/// rule; this is shell integration, not rendering.
///
/// Messages arrive on this thread's queue and are dispatched by
/// `glfwPollEvents()`, which pumps every message for the calling thread. That is
/// why there is deliberately no second message loop here: one pump, owned by the
/// frame loop, so the tray and the window cannot disagree about the state.
///
/// On a platform without a notification area `create()` returns false, and the
/// caller must then keep its window as the only way to quit.
class TrayIcon {
public:
	using Handler = std::function<void(TrayAction)>;

	TrayIcon();
	~TrayIcon();

	TrayIcon(const TrayIcon&) = delete;
	TrayIcon& operator=(const TrayIcon&) = delete;

	/// Add the icon and start listening. `handler` runs on the frame loop's
	/// thread, from inside `glfwPollEvents()`.
	TrayResult create(const std::string& tooltip, Handler handler);

	/// Remove the icon and the message window. Idempotent.
	void destroy();

	bool valid() const;

	/// Hover text. Only touches the shell when the text actually changed, so the
	/// frame loop can call it every frame.
	void setTooltip(const std::string& tooltip);

	/// What the menu shows the next time it opens.
	void setState(const TrayState& state);

	/// Run one action now, exactly as the menu would.
	void act(TrayAction action);

	/// Win32 state: the hidden window, the icon, the single-instance mutex and
	/// the menu. Declared here only because the window procedure — a C callback
	/// in TrayIcon.cpp — has to name the type; it is **incomplete** in this
	/// header, so none of the Win32 detail reaches anything that includes it.
	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
};

} // namespace media
