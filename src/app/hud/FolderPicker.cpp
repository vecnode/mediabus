#include "app/hud/FolderPicker.h"

#include "core/Log.h"

#include <system_error>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// The modern picker, IFileOpenDialog. Note the header name: MinGW ships
// shobjidl.h (the full IDL-generated interface) and not the SDK's split
// shobjidl_core.h, so this is the portable spelling of the same declarations.
// It pulls in ole2.h itself for IFileDialog's IUnknown base.
#include <shobjidl.h>

#include <cwchar>
#include <filesystem>
#include <string>

namespace media::ui {
namespace fs = std::filesystem;

namespace {

std::wstring widen(const std::string& text) {
	if (text.empty()) {
		return {};
	}
	const int length = static_cast<int>(text.size());
	const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), length, nullptr, 0);
	if (size <= 0) {
		return {};
	}
	std::wstring wide(static_cast<std::size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), length, &wide[0], size);
	return wide;
}

std::string narrow(const wchar_t* text) {
	if (text == nullptr || text[0] == L'\0') {
		return {};
	}
	const int length = static_cast<int>(wcslen(text));
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0,
		nullptr, nullptr);
	if (size <= 0) {
		return {};
	}
	std::string out(static_cast<std::size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, length, &out[0], size, nullptr, nullptr);
	return out;
}

/// Owns the COM apartment for the duration of one call, and only releases it
/// if this function was the one that initialised it.
class ComScope {
public:
	ComScope() {
		const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED
			| COINIT_DISABLE_OLE1DDE);
		// S_FALSE means "already initialised on this thread with the same
		// model" - not an error, and not ours to uninitialise.
		// RPC_E_CHANGED_MODE means it is initialised as MTA; the picker still
		// works, and uninitialising would be wrong.
		owned_ = (hr == S_OK);
		usable_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
		if (!usable_) {
			LOG_WARN("FolderPicker") << "CoInitializeEx failed (0x" << std::hex << hr
				<< std::dec << "); no folder picker";
		}
	}

	~ComScope() {
		if (owned_) {
			CoUninitialize();
		}
	}

	ComScope(const ComScope&) = delete;
	ComScope& operator=(const ComScope&) = delete;

	bool usable() const { return usable_; }

private:
	bool owned_ = false;
	bool usable_ = false;
};

/// COM pointer with the release-on-drop behaviour this file needs, without
/// pulling in a smart-pointer library.
template <typename T>
class ComPtr {
public:
	~ComPtr() {
		if (ptr_ != nullptr) {
			ptr_->Release();
		}
	}

	ComPtr(const ComPtr&) = delete;
	ComPtr& operator=(const ComPtr&) = delete;
	ComPtr() = default;

	T** put() { return &ptr_; }
	T* get() const { return ptr_; }
	explicit operator bool() const { return ptr_ != nullptr; }

private:
	T* ptr_ = nullptr;
};

} // namespace

bool folderPickerAvailable() {
	return true;
}

std::string pickFolder(const std::string& title, const std::string& initialDirectory,
	bool* cancelled) {
	if (cancelled != nullptr) {
		*cancelled = false;
	}

	ComScope com;
	if (!com.usable()) {
		// Reported as "cancelled" by contract: the caller has nothing different
		// to do, and leaving `cancelled` false tells a caller that asks that
		// this was not a user decision.
		return {};
	}

	ComPtr<IFileOpenDialog> dialog;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(dialog.put()));
	if (FAILED(hr) || !dialog) {
		LOG_WARN("FolderPicker") << "CoCreateInstance(FileOpenDialog) failed (0x"
			<< std::hex << hr << std::dec << ")";
		return {};
	}

	DWORD options = 0;
	if (SUCCEEDED(dialog.get()->GetOptions(&options))) {
		// FOS_FORCEFILESYSTEM keeps the result a real path rather than a
		// virtual shell item (a library, "This PC"), which the Player could not
		// scan. FOS_PATHMUSTEXIST because a corpus folder that is not there is
		// never what was meant.
		dialog.get()->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM
			| FOS_PATHMUSTEXIST);
	}

	const std::wstring wideTitle = widen(title.empty() ? "Select the media folder" : title);
	if (!wideTitle.empty()) {
		dialog.get()->SetTitle(wideTitle.c_str());
	}

	// Seed the dialog with the folder in use, so "change folder" starts where
	// the user already is. A path that no longer exists is simply skipped.
	if (!initialDirectory.empty()) {
		std::error_code ec;
		if (fs::is_directory(initialDirectory, ec)) {
			ComPtr<IShellItem> start;
			const std::wstring wideStart = widen(initialDirectory);
			if (!wideStart.empty()
				&& SUCCEEDED(SHCreateItemFromParsingName(wideStart.c_str(), nullptr,
					IID_PPV_ARGS(start.put())))) {
				dialog.get()->SetFolder(start.get());
			}
		}
	}

	hr = dialog.get()->Show(nullptr);
	if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
		if (cancelled != nullptr) {
			*cancelled = true;
		}
		return {};
	}
	if (FAILED(hr)) {
		LOG_WARN("FolderPicker") << "the folder dialog failed (0x" << std::hex << hr
			<< std::dec << ")";
		return {};
	}

	ComPtr<IShellItem> item;
	if (FAILED(dialog.get()->GetResult(item.put())) || !item) {
		LOG_WARN("FolderPicker") << "the folder dialog returned no result";
		return {};
	}

	PWSTR raw = nullptr;
	if (FAILED(item.get()->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
		// Reached for a shell item with no filesystem path. FOS_FORCEFILESYSTEM
		// should have prevented it.
		LOG_WARN("FolderPicker") << "the chosen item has no filesystem path";
		return {};
	}
	std::string chosen = narrow(raw);
	CoTaskMemFree(raw);

	if (chosen.empty()) {
		return {};
	}
	// Normalise to preferred separators so the value written to
	// mediaplayer.ini is the same string the library will report back.
	std::error_code ec;
	fs::path path(chosen);
	path.make_preferred();
	chosen = path.string();
	LOG_NOTICE("FolderPicker") << "chosen: " << chosen;
	return chosen;
}

} // namespace media::ui

#else   // not _WIN32

namespace media::ui {

bool folderPickerAvailable() {
	return false;
}

std::string pickFolder(const std::string&, const std::string&, bool* cancelled) {
	if (cancelled != nullptr) {
		*cancelled = false;
	}
	LOG_WARN("FolderPicker") << "no native folder picker in this build";
	return {};
}

} // namespace media::ui

#endif
