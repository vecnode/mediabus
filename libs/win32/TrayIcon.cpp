#include "win32/TrayIcon.h"

#include "core/Log.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <utility>

namespace media {
namespace {

/// The message the shell sends for clicks and menu events: WM_APP is the first
/// value an application may use for its own messages.
constexpr UINT kCallbackMessage = WM_APP + 1;

constexpr UINT kMenuLaunchPlayer = 1;
constexpr UINT kMenuLaunchController = 2;
constexpr UINT kMenuStopPlayer = 3;
constexpr UINT kMenuStopController = 4;
constexpr UINT kMenuToggleWindow = 5;
constexpr UINT kMenuQuit = 6;

/// IDI_APPLICATION, spelled as a number so it can be loaded with the W function
/// whatever UNICODE is set to.
constexpr int kIdiApplication = 32512;

constexpr wchar_t kWindowClass[] = L"MediaPlayerAppLauncherTray";
/// One launcher per session. A second one would put a second icon in the tray
/// and could start a second Player on a port that is already taken.
constexpr wchar_t kInstanceMutex[] = L"Local\\vn-mediabus-launcher";

std::wstring widen(const std::string& text) {
	if (text.empty()) {
		return std::wstring();
	}
	const int length = static_cast<int>(text.size());
	const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), length, nullptr, 0);
	if (size <= 0) {
		return std::wstring();
	}
	std::wstring wide(static_cast<std::size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), length, &wide[0], size);
	return wide;
}

/// The product icon, loaded by the *name* the .rc files give it. They say
/// `IDI_APPICON ICON "..."` with no `#define`, and windres takes an undefined
/// identifier as a resource name — so the executables have no numeric icon id at
/// all, and an ordinal lookup returns NULL and leaves the tray button empty.
HICON loadAppIcon() {
	if (HICON icon = LoadIconW(GetModuleHandleW(nullptr), L"IDI_APPICON")) {
		return icon;
	}
	LOG_WARN("Tray") << "no IDI_APPICON resource; falling back to the generic "
		"application icon";
	return LoadIconW(nullptr, MAKEINTRESOURCEW(kIdiApplication));
}

TrayAction actionForCommand(UINT command) {
	switch (command) {
		case kMenuLaunchPlayer: return TrayAction::LaunchPlayer;
		case kMenuLaunchController: return TrayAction::LaunchController;
		case kMenuStopPlayer: return TrayAction::StopPlayer;
		case kMenuStopController: return TrayAction::StopController;
		case kMenuToggleWindow: return TrayAction::ToggleWindow;
		case kMenuQuit: return TrayAction::Quit;
		default: return TrayAction::None;
	}
}

} // namespace

struct TrayIcon::Impl {
	HWND window = nullptr;
	HICON icon = nullptr;
	HANDLE instance = nullptr;
	UINT taskbarCreated = 0;

	NOTIFYICONDATAW data{};
	bool added = false;

	TrayState state;
	std::string tooltip;
	TrayIcon::Handler handler;
	TrayIcon* owner = nullptr;

	void addIcon() {
		data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
		data.uCallbackMessage = kCallbackMessage;
		data.hIcon = icon;

		const std::wstring tip = widen(tooltip);
		std::size_t i = 0;
		for (; i < tip.size() && i < 127; ++i) {
			data.szTip[i] = tip[i];
		}
		data.szTip[i] = L'\0';

		if (Shell_NotifyIconW(added ? NIM_MODIFY : NIM_ADD, &data) == FALSE) {
			LOG_WARN("Tray") << "Shell_NotifyIcon failed (error " << GetLastError()
				<< "); there is no tray icon";
			added = false;
			// The single-instance mutex deliberately stays held even though the
			// icon did not appear. A second launcher is never useful — it would
			// manage the same two applications again — so the failure to get an
			// icon must not quietly turn the next launch into a second one.
			return;
		}
		added = true;
	}

	void removeIcon() {
		if (added) {
			Shell_NotifyIconW(NIM_DELETE, &data);
			added = false;
		}
	}

	/// The menu, built from the state the frame loop last published. Disabled
	/// items are greyed rather than hidden, so the menu does not change shape
	/// under the pointer.
	void showMenu() {
		HMENU menu = CreatePopupMenu();
		if (menu == nullptr) {
			return;
		}
		AppendMenuW(menu, MF_STRING, kMenuLaunchPlayer, L"Launch Player");
		AppendMenuW(menu, MF_STRING, kMenuLaunchController, L"Launch Controller");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING | (state.playerManaged ? MF_ENABLED : MF_GRAYED),
			kMenuStopPlayer, L"Stop Player");
		AppendMenuW(menu, MF_STRING | (state.controllerManaged ? MF_ENABLED : MF_GRAYED),
			kMenuStopController, L"Stop Controller");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuToggleWindow,
			state.windowVisible ? L"Hide Dashboard" : L"Show Dashboard");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");
		SetMenuDefaultItem(menu, kMenuQuit, FALSE);

		POINT cursor{};
		GetCursorPos(&cursor);
		// Both of these are required, not cosmetic. Without SetForegroundWindow
		// the menu does not dismiss when the user clicks elsewhere, and the
		// trailing WM_NULL is the documented workaround for the first click
		// being swallowed.
		SetForegroundWindow(window);
		const UINT command = TrackPopupMenu(menu,
			TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN,
			cursor.x, cursor.y, 0, window, nullptr);
		PostMessageW(window, WM_NULL, 0, 0);
		DestroyMenu(menu);

		const TrayAction action = actionForCommand(command);
		if (action != TrayAction::None && owner != nullptr) {
			owner->act(action);
		}
	}
};

namespace {

LRESULT CALLBACK trayWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	auto* impl = reinterpret_cast<TrayIcon::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
		impl = static_cast<TrayIcon::Impl*>(create->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
	}
	if (impl == nullptr) {
		return DefWindowProcW(hwnd, message, wparam, lparam);
	}

	// Explorer restarting destroys every icon in the tray and broadcasts this
	// message afterwards; the documented answer is to add the icon again. This
	// is why the window below is a hidden top-level window and not a
	// message-only one: message-only windows receive no broadcasts.
	if (impl->taskbarCreated != 0 && message == impl->taskbarCreated) {
		impl->added = false;
		impl->addIcon();
		return 0;
	}

	switch (message) {
		case kCallbackMessage:
			switch (LOWORD(lparam)) {
				case WM_LBUTTONUP:
				case WM_LBUTTONDBLCLK:
					if (impl->owner != nullptr) {
						impl->owner->act(TrayAction::ToggleWindow);
					}
					break;
				case WM_RBUTTONUP:
				case WM_CONTEXTMENU:
					impl->showMenu();
					break;
				default:
					break;
			}
			return 0;

		case WM_DESTROY:
			impl->removeIcon();
			return 0;

		// The menu below asks for its result (TPM_RETURNCMD), so it does not
		// send this — but a posted WM_COMMAND is a documented way to drive the
		// same actions from outside, which is how the tray is verified without
		// anyone clicking. Both paths land in act(), so they cannot diverge.
		case WM_COMMAND:
			if (impl->owner != nullptr) {
				impl->owner->act(actionForCommand(LOWORD(wparam)));
			}
			return 0;

		default:
			return DefWindowProcW(hwnd, message, wparam, lparam);
	}
}

} // namespace

TrayIcon::TrayIcon() : impl_(new Impl()) {}

TrayIcon::~TrayIcon() {
	destroy();
	// impl_ is a unique_ptr, so the Impl goes with it.
}

TrayResult TrayIcon::create(const std::string& tooltip, Handler handler) {
	if (impl_->window != nullptr) {
		return impl_->added ? TrayResult::Created : TrayResult::Unavailable;
	}

	// One launcher per session, and this is what keeps a second tray icon from
	// appearing. The handle is held for the process's lifetime on purpose.
	impl_->instance = CreateMutexW(nullptr, FALSE, kInstanceMutex);
	if (impl_->instance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
		LOG_NOTICE("Tray") << "another launcher already owns the tray";
		return TrayResult::AlreadyRunning;
	}

	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = trayWindowProc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = kWindowClass;
	if (RegisterClassExW(&windowClass) == 0
		&& GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		LOG_WARN("Tray") << "RegisterClassEx failed: " << GetLastError();
		return TrayResult::Unavailable;
	}

	impl_->handler = std::move(handler);
	impl_->owner = this;
	impl_->tooltip = tooltip;
	impl_->icon = loadAppIcon();
	impl_->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

	impl_->window = CreateWindowExW(0, kWindowClass, L"vn-mediabus launcher", 0,
		0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), impl_.get());
	if (impl_->window == nullptr) {
		LOG_WARN("Tray") << "CreateWindowEx failed: " << GetLastError();
		impl_->owner = nullptr;
		return TrayResult::Unavailable;
	}

	impl_->data.cbSize = sizeof(NOTIFYICONDATAW);
	impl_->data.hWnd = impl_->window;
	impl_->data.uID = 1;
	impl_->addIcon();

	if (impl_->added) {
		LOG_NOTICE("Tray") << "launcher icon added to the notification area";
		return TrayResult::Created;
	}
	return TrayResult::Unavailable;
}

void TrayIcon::destroy() {
	if (impl_ == nullptr || impl_->window == nullptr) {
		return;
	}
	impl_->removeIcon();
	DestroyWindow(impl_->window);
	impl_->window = nullptr;
	impl_->owner = nullptr;
	impl_->handler = nullptr;
	if (impl_->instance != nullptr) {
		CloseHandle(impl_->instance);
		impl_->instance = nullptr;
	}
}

bool TrayIcon::valid() const {
	return impl_ != nullptr && impl_->added;
}

void TrayIcon::setTooltip(const std::string& tooltip) {
	if (impl_ == nullptr || tooltip == impl_->tooltip) {
		return;
	}
	impl_->tooltip = tooltip;
	if (impl_->added) {
		impl_->addIcon();   // NIM_MODIFY: the icon is already there
	}
}

void TrayIcon::setState(const TrayState& state) {
	if (impl_ != nullptr) {
		impl_->state = state;
	}
}

void TrayIcon::act(TrayAction action) {
	if (impl_ != nullptr && impl_->handler) {
		impl_->handler(action);
	}
}

} // namespace media

#else   // not _WIN32

namespace media {

// No notification area on this platform: the launcher stays an ordinary window,
// and the caller must keep its window as the only way to quit.
struct TrayIcon::Impl {};

TrayIcon::TrayIcon() : impl_(new Impl()) {}
TrayIcon::~TrayIcon() = default;

TrayResult TrayIcon::create(const std::string&, Handler) { return TrayResult::Unsupported; }
void TrayIcon::destroy() {}
bool TrayIcon::valid() const { return false; }
void TrayIcon::setTooltip(const std::string&) {}
void TrayIcon::setState(const TrayState&) {}
void TrayIcon::act(TrayAction) {}

} // namespace media

#endif
