#pragma once

/// The ImGui fence.
///
/// HARD RULE, the same shape as RenderDevice's: no application file and no
/// library below this one may include <imgui.h> or name an OpenGL symbol. They
/// talk to UiLayer, which owns the ImGui context, advances the frame, loads the
/// fonts and submits the draw data. Everything in THIS header is plain C++:
/// no ImGui type, no GL type, no macro.
///
/// Why a fence at all rather than calling ImGui directly from the two apps:
/// Dear ImGui is a graphics API - it owns GL objects and GL shaders - so a
/// stray #include <imgui.h> in a main file would break the one rule that lets
/// this project replace its renderer without rewriting its applications. The
/// same reasoning puts MPVSurface in a library of its own.
///
/// The two applications that use this:
///   - the Controller, whose bar is now an ImGui panel;
///   - the Dashboard, whose launcher plus Lua editor is an ImGui panel.
/// The Player deliberately does not link it: it composites video every frame
/// and draws its own 5x7 HUD through RenderDevice.
///
/// Threading: one UiLayer per process, created and used only on the thread that
/// owns the GL context. ImGui is not thread-safe and does not pretend to be.

#include <cstdint>
#include <memory>
#include <string>

namespace media::ui {

/// The palette. Every colour an application draws with comes from here, so the
/// two ImGui applications cannot drift apart visually, and so a theme change is
/// one struct rather than a search-and-replace.
struct UiTheme {
	/// Window background.
	std::uint32_t background = 0x101319;
	/// A panel or card sitting on that background.
	std::uint32_t panel = 0x181C24;
	/// A raised element inside a panel (a header, a selected row).
	std::uint32_t panelAlt = 0x212734;
	/// Frames, separators and borders.
	std::uint32_t border = 0x2C3442;
	/// The accent: primary buttons, the active tab, the seek fill.
	std::uint32_t accent = 0x4B8BBE;
	/// The accent's hovered and pressed states.
	std::uint32_t accentHover = 0x5FA3D6;
	std::uint32_t accentActive = 0x3A76A6;
	/// Primary text.
	std::uint32_t text = 0xE6EAF2;
	/// Secondary text: labels, hints, a dimmed path.
	std::uint32_t textDim = 0x8A93A5;
	/// A state that matters but is not a failure: paused, offline, a warning.
	std::uint32_t warn = 0xD9A441;
	/// A refusal or a failed probe.
	std::uint32_t error = 0xD9534F;
	/// Running, online, applied.
	std::uint32_t ok = 0x5CB85C;

	/// Corner rounding of panels, buttons and frames, in logical points.
	float rounding = 4.0f;
	/// Width of a frame or panel border, in logical points.
	float borderSize = 1.0f;
	/// Gap between widgets vertically, in logical points.
	float spacing = 8.0f;
	/// Inner padding of a panel or window.
	float padding = 12.0f;
};

/// Where the interface's glyphs come from.
///
/// The default is a font Windows always has, as the plan called for: Segoe UI
/// for the interface and Consolas for numbers and the code editor. Nothing is
/// bundled, so the binary does not carry a font and the text matches the rest
/// of the desktop. If neither file is found the built-in ImGui font is used and
/// the log says so - the interface is never blank and never refuses to start.
struct UiFonts {
	/// Absolute path to the interface font. Empty asks for the system default.
	std::string uiPath;
	/// Absolute path to the monospace font. Empty asks for the system default.
	std::string monoPath;
	/// Interface font size, in logical points at 100% scaling.
	float uiSizePts = 16.0f;
	/// Monospace size, in logical points at 100% scaling. The code editor and
	/// the numeric readouts use this one.
	float monoSizePts = 15.0f;
	/// True when a real system font was found and loaded; false means the
	/// built-in fallback is in use.
	bool systemFontLoaded = false;
	/// What was actually loaded, for the startup log line.
	std::string uiSource;
	std::string monoSource;
};

/// Everything the layer needs to know about one frame. All sizes are in
/// FRAMEBUFFER pixels, not window coordinates: the Dashboard has no
/// window-to-framebuffer correction, so on a scaled display the two are
/// different numbers and feeding ImGui the window size would leave it drawing
/// against a viewport the rest of the frame does not share.
struct UiFrameInfo {
	int framebufferWidth = 0;
	int framebufferHeight = 0;
	/// Monitor content scale. Scales spacing and fonts together, so a 4K panel
	/// at 100% Windows scaling gets readable text rather than a 7-pixel capital.
	float uiScale = 1.0f;
	/// Seconds since the previous frame. Used for animation only; the interface
	/// is immediate-mode and has no state of its own to advance.
	double deltaSeconds = 0.0;
};

class UiLayer {
public:
	/// Create the ImGui context and both backends against `glfwWindow`.
	///
	/// `glfwWindow` is a GLFWwindow*, passed as void* so this header does not
	/// pull GLFW in either. The GL context must already be current on this
	/// thread. Returns nullptr when ImGui could not start at all - which is not
	/// fatal for either application: both can fall back to their previous
	/// RenderDevice interface, and both say so in the log.
	static std::unique_ptr<UiLayer> create(void* glfwWindow);

	~UiLayer();

	UiLayer(const UiLayer&) = delete;
	UiLayer& operator=(const UiLayer&) = delete;

	bool ready() const { return ready_; }

	/// Apply the theme and load the fonts. Called once by the constructor path;
	/// exposed so an application can restyle before its first frame.
	void applyTheme(const UiTheme& theme);
	const UiTheme& theme() const { return theme_; }

	/// Replace the fonts. Safe to call before the first beginFrame() or between
	/// frames; the atlas is rebuilt and the old one released.
	void setFonts(const UiFonts& fonts);
	const UiFonts& fonts() const { return fonts_; }

	/// The names the loaded fonts are registered under, for ImGui::PushFont.
	/// Empty when the fallback font is in use.
	const char* uiFontName() const { return kUiFontName; }
	const char* monoFontName() const { return kMonoFontName; }

	/// Set the interface scale. Multiplies spacing and font size together, so
	/// the two can never disagree. The value comes from core/UiScale, which is
	/// also what the Player's bitmap HUD uses.
	void setUiScale(float scale);
	float uiScale() const { return uiScale_; }

	/// Where ImGui persists window positions and sizes. Set before the first
	/// frame. Empty disables persistence entirely.
	void setIniPath(std::string path);

	/// Let ImGui install its GLFW callbacks. It chains whatever callbacks were
	/// registered before, so an application's own handlers keep firing.
	void installCallbacks();

	/// Start a frame: both backend NewFrame calls, then ImGui::NewFrame().
	/// Must be paired with exactly one endFrame().
	void beginFrame(const UiFrameInfo& info);

	/// Finish the frame: ImGui::Render() and the OpenGL3 draw-data submission.
	/// Leaves GL state as the RenderDevice expects to find it, so the order
	/// between this and RenderDevice::endFrame() does not matter.
	void endFrame();

	/// True when ImGui owns the pointer this frame. An application should skip
	/// its own click handling while this is true, or the interface eats clicks
	/// that another handler would also act on.
	bool wantsMouse() const;

	/// True when ImGui owns the keyboard this frame (a text field has focus).
	/// An application should skip its hotkeys while this is true.
	bool wantsKeyboard() const;

	/// Tear down the GL objects and the context. MUST run while the GL context
	/// is still current, which means before the window is destroyed and before
	/// the RenderDevice is reset.
	void shutdown();

	/// One line of identity for the startup log and /api/status diagnostics.
	std::string describe() const;

	/// The name the interface font is registered under.
	static constexpr const char* kUiFontName = "ui";
	/// The name the monospace font is registered under.
	static constexpr const char* kMonoFontName = "mono";

private:
	UiLayer() = default;

	/// Find a usable system font, or leave the built-in one in place.
	void loadFonts();

	/// Push spacing, rounding and colours into ImGuiStyle and ImGui::GetStyle.
	void applyThemeToStyle();

	struct Impl;
	std::unique_ptr<Impl> impl_;

	UiTheme theme_{};
	UiFonts fonts_{};
	float uiScale_ = 1.0f;
	bool ready_ = false;
	bool fontsLoaded_ = false;
	bool frameOpen_ = false;
};

/// Where Windows keeps its fonts. Empty on a platform without one, which is the
/// signal to use the built-in font.
std::string systemFontDirectory();

/// The first font that exists, in preference order, from `systemFontDirectory`.
/// `monospace` selects the fixed-pitch list. Returns an empty string when none
/// of the candidates exist - which is a valid outcome, not an error.
std::string findSystemFont(bool monospace);

} // namespace media::ui
