#pragma once

#include <string>

namespace media::ui {

/// Ask the user for a folder with the operating system's own picker.
///
/// The Controller and the Dashboard both need "pick the media corpus folder".
/// Drawing a directory browser out of quads would be slower to use and a lot
/// more code than this, so the OS dialog is used and the platform detail is
/// confined to this one translation unit - the same treatment
/// app/dashboard/TrayIcon.cpp gets for the notification area.
///
/// `initialDirectory` seeds the dialog when it exists; an empty or missing one
/// falls back to the dialog's own default location.
///
/// Returns:
///   - the chosen folder, absolute, on OK;
///   - an empty string when the user cancelled *or* no picker is available on
///     this platform, which are deliberately the same answer: a caller has
///     nothing different to do about either. `cancelled` distinguishes them for
///     the one case that cares (reporting "the picker is not available here").
///
/// Must be called on the thread that owns the window, and never from an HTTP
/// worker thread: it put a modal dialog on screen and runs its own message
/// loop. The Controller calls it from the frame loop in response to a click.
std::string pickFolder(const std::string& title, const std::string& initialDirectory,
	bool* cancelled = nullptr);

/// Whether this build has a folder picker at all.
bool folderPickerAvailable();

} // namespace media::ui
