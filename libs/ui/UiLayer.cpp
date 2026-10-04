#include "ui/UiLayer.h"

#include "core/Log.h"

// The ImGui fence in practice: this is one of only two translation units in the
// project that includes <imgui.h>, and the only one outside vendor/. Nothing
// above this file names ImGui or OpenGL.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// GLEW is the loader for the whole process. The backend is told to use it via
// IMGUI_IMPL_OPENGL_LOADER_CUSTOM (see the imgui target in CMakeLists.txt), and
// this include is why: without it the backend would define its own copy of the
// same function pointers under the same names.
#define GLFW_INCLUDE_NONE
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace media::ui {
namespace fs = std::filesystem;

namespace {

/// The GLSL version both the RenderDevice and this backend compile against.
/// Pinned rather than left to the backend's "highest available" probe: the
/// device's own shaders are `#version 330 core`, and two different versions in
/// one frame is a difference with no benefit.
constexpr const char* kGlslVersion = "#version 330 core";

/// The toolbar a font is rasterised with. 2x keeps the glyphs crisp when ImGui
/// scales them up for a high-DPI monitor; the atlas stays small because only
/// the ranges in buildGlyphRanges() are rasterised.
constexpr int kFontOversample = 2;

ImVec4 toImVec4(std::uint32_t rgb, float alpha = 1.0f) {
	return ImVec4(
		static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
		static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
		static_cast<float>(rgb & 0xFFu) / 255.0f,
		alpha);
}

/// Build a glyph range covering Latin, Latin-1 Supplement and the punctuation
/// and arrows the interface actually uses. Deliberately not GetGlyphRangesDefault
/// (Basic Latin only): a folder path with an accented character, or a clip name
/// with a typographic quote, would render as a blank box.
ImVector<ImWchar> buildGlyphRanges() {
	ImVector<ImWchar> ranges;
	ImFontGlyphRangesBuilder builder;
	builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
	// Latin-1 Supplement and Latin Extended-A: most European accented letters
	// that appear in file and folder names.
	static const ImWchar latin[] = {0x00A0, 0x017F, 0};
	builder.AddRanges(latin);
	// Arrows, bullets and geometric shapes: transport glyphs and status dots.
	static const ImWchar symbols[] = {0x2022, 0x2022, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
	builder.AddRanges(symbols);
	builder.BuildRanges(&ranges);
	return ranges;
}

} // namespace

// ---------------------------------------------------------------------------
// System font discovery
// ---------------------------------------------------------------------------

std::string systemFontDirectory() {
#if defined(_WIN32)
	// GetWindowsDirectoryW rather than a hard-coded C:\Windows, so a system on
	// another drive still resolves.
	wchar_t buffer[MAX_PATH] = {0};
	const UINT written = GetWindowsDirectoryW(buffer, MAX_PATH);
	if (written == 0) {
		return {};
	}
	fs::path dir(buffer);
	dir /= L"Fonts";
	return dir.string();
#else
	// Not a target platform. An empty result is the documented signal to use
	// ImGui's built-in font, which is what a build here would want anyway.
	return {};
#endif
}

std::string findSystemFont(bool monospace) {
	const std::string dir = systemFontDirectory();
	if (dir.empty()) {
		return {};
	}
	// Preference order within each family. Segoe UI is the Windows shell font,
	// so the interface reads as part of the desktop rather than as an app that
	// brought its own typeface; Consolas is the monospace font that ships with
	// every supported Windows and has clean digit shapes for the readouts.
	static const char* kUiCandidates[] = {
		"segoeui.ttf",      // Segoe UI, the shell font
		"tahoma.ttf",       // older and smaller, but present everywhere
		"verdana.ttf",
		"arial.ttf",
		"calibri.ttf",
	};
	static const char* kMonoCandidates[] = {
		"consola.ttf",      // Consolas
		"cour.ttf",         // Courier New, the last resort
		"lucon.ttf",        // Lucida Console
	};
	const char** begin = monospace ? kMonoCandidates : kUiCandidates;
	const std::size_t count = monospace
		? (sizeof(kMonoCandidates) / sizeof(kMonoCandidates[0]))
		: (sizeof(kUiCandidates) / sizeof(kUiCandidates[0]));

	for (std::size_t i = 0; i < count; ++i) {
		fs::path candidate = fs::path(dir) / begin[i];
		std::error_code ec;
		if (fs::exists(candidate, ec) && !ec) {
			return candidate.string();
		}
	}
	return {};
}

// ---------------------------------------------------------------------------
// UiLayer
// ---------------------------------------------------------------------------

struct UiLayer::Impl {
	GLFWwindow* window = nullptr;
	/// A pristine copy of ImGui's default style. setUiScale() rebuilds from this
	/// rather than scaling the live style, because ScaleAllSizes multiplies: two
	/// calls at 1.5 would leave spacing at 2.25x.
	ImGuiStyle baseStyle{};
	bool baseStyleValid = false;
	/// The atlas ranges, built once. Stored rather than rebuilt so the pointer
	/// handed to AddFontFromFileTTF stays valid for the lifetime of the atlas.
	ImVector<ImWchar> glyphRanges;
	std::string iniPath;
};

std::unique_ptr<UiLayer> UiLayer::create(void* glfwWindow) {
	if (glfwWindow == nullptr) {
		return nullptr;
	}

	// IMGUI_CHECKVERSION() is an assert on the compiled and header versions
	// agreeing, which catches a half-updated vendored copy. It is checked here
	// by hand so the message says what to do rather than just asserting.
	if (std::strcmp(ImGui::GetVersion(), IMGUI_VERSION) != 0) {
		LOG_ERROR("UI") << "ImGui version mismatch: header " << IMGUI_VERSION
			<< ", compiled " << ImGui::GetVersion();
		return nullptr;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	auto layer = std::unique_ptr<UiLayer>(new UiLayer());
	layer->impl_ = std::make_unique<Impl>();
	layer->impl_->window = static_cast<GLFWwindow*>(glfwWindow);

	ImGuiIO& io = ImGui::GetIO();
	// Keyboard navigation on: every action in this interface is reachable
	// without a mouse, which matters for a control panel an operator drives
	// next to a video.
	//
	// Docking and multi-viewport are deliberately NOT enabled. This is the
	// non-docking release of Dear ImGui, so neither flag exists here at all;
	// two windows and a child editor do not want them anyway.
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
	io.ConfigFlags &= ~ImGuiConfigFlags_NoKeyboard;

	layer->impl_->baseStyle = ImGui::GetStyle();
	layer->impl_->baseStyleValid = true;
	layer->ready_ = true;

	layer->applyThemeToStyle();

	// GDK/GLFW backends. install_callbacks=false: the applications install
	// their own handlers and ask this layer what ImGui wants, which keeps the
	// decision about who owns an input in the application rather than split
	// between a chained callback and the frame loop.
	if (!ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(glfwWindow), false)) {
		LOG_ERROR("UI") << "ImGui_ImplGlfw_InitForOpenGL failed";
		ImGui::DestroyContext();
		return nullptr;
	}
	if (!ImGui_ImplOpenGL3_Init(kGlslVersion)) {
		LOG_ERROR("UI") << "ImGui_ImplOpenGL3_Init failed for " << kGlslVersion;
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		return nullptr;
	}

	LOG_NOTICE("UI") << "ImGui " << ImGui::GetVersion() << " (" << kGlslVersion
		<< ", GLEW loader)";
	return layer;
}

UiLayer::~UiLayer() {
	// shutdown() is idempotent; this only catches a caller that forgot, in
	// which case the GL objects may already be gone and there is nothing safe
	// left to do but release the context if it is still there.
	if (ready_ || impl_) {
		shutdown();
	}
}

void UiLayer::applyTheme(const UiTheme& theme) {
	theme_ = theme;
	applyThemeToStyle();
}

void UiLayer::applyThemeToStyle() {
	if (!ready_) {
		return;
	}
	ImGuiStyle& style = ImGui::GetStyle();

	style.WindowRounding = theme_.rounding;
	style.ChildRounding = theme_.rounding;
	style.FrameRounding = theme_.rounding * 0.75f;
	style.PopupRounding = theme_.rounding;
	style.ScrollbarRounding = theme_.rounding;
	style.GrabRounding = theme_.rounding * 0.75f;
	style.TabRounding = theme_.rounding * 0.75f;
	style.WindowBorderSize = theme_.borderSize;
	style.ChildBorderSize = theme_.borderSize;
	style.FrameBorderSize = 0.0f;   // frames read better with a filled contrast
	style.PopupBorderSize = theme_.borderSize;
	style.WindowPadding = ImVec2(theme_.padding, theme_.padding);
	style.FramePadding = ImVec2(theme_.padding * 0.6f, theme_.spacing * 0.45f);
	style.ItemSpacing = ImVec2(theme_.spacing, theme_.spacing * 0.75f);
	style.ItemInnerSpacing = ImVec2(theme_.spacing * 0.5f, theme_.spacing * 0.5f);
	style.IndentSpacing = theme_.spacing * 2.0f;
	style.ScrollbarSize = theme_.spacing * 1.6f;
	style.GrabMinSize = theme_.spacing;

	ImVec4* c = style.Colors;
	c[ImGuiCol_Text]                  = toImVec4(theme_.text);
	c[ImGuiCol_TextDisabled]          = toImVec4(theme_.textDim);
	c[ImGuiCol_WindowBg]              = toImVec4(theme_.background);
	c[ImGuiCol_ChildBg]               = toImVec4(theme_.panel, 0.0f);
	c[ImGuiCol_PopupBg]               = toImVec4(theme_.panelAlt, 0.98f);
	c[ImGuiCol_Border]                = toImVec4(theme_.border);
	c[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	c[ImGuiCol_FrameBg]               = toImVec4(theme_.panelAlt);
	c[ImGuiCol_FrameBgHovered]        = toImVec4(theme_.border);
	c[ImGuiCol_FrameBgActive]         = toImVec4(theme_.border);
	c[ImGuiCol_TitleBg]               = toImVec4(theme_.background);
	c[ImGuiCol_TitleBgActive]         = toImVec4(theme_.panel);
	c[ImGuiCol_TitleBgCollapsed]      = toImVec4(theme_.background);
	c[ImGuiCol_MenuBarBg]             = toImVec4(theme_.panel);
	c[ImGuiCol_ScrollbarBg]           = toImVec4(theme_.background, 0.0f);
	c[ImGuiCol_ScrollbarGrab]         = toImVec4(theme_.border);
	c[ImGuiCol_ScrollbarGrabHovered]  = toImVec4(theme_.textDim);
	c[ImGuiCol_ScrollbarGrabActive]   = toImVec4(theme_.accent);
	c[ImGuiCol_CheckMark]             = toImVec4(theme_.accent);
	c[ImGuiCol_SliderGrab]            = toImVec4(theme_.accent);
	c[ImGuiCol_SliderGrabActive]      = toImVec4(theme_.accentHover);
	c[ImGuiCol_Button]                = toImVec4(theme_.panelAlt);
	c[ImGuiCol_ButtonHovered]         = toImVec4(theme_.border);
	c[ImGuiCol_ButtonActive]          = toImVec4(theme_.accent);
	c[ImGuiCol_Header]                = toImVec4(theme_.panelAlt);
	c[ImGuiCol_HeaderHovered]         = toImVec4(theme_.border);
	c[ImGuiCol_HeaderActive]          = toImVec4(theme_.accent);
	c[ImGuiCol_Separator]             = toImVec4(theme_.border);
	c[ImGuiCol_SeparatorHovered]      = toImVec4(theme_.accent);
	c[ImGuiCol_SeparatorActive]       = toImVec4(theme_.accentHover);
	c[ImGuiCol_ResizeGrip]            = toImVec4(theme_.border, 0.0f);
	c[ImGuiCol_ResizeGripHovered]     = toImVec4(theme_.accent, 0.6f);
	c[ImGuiCol_ResizeGripActive]      = toImVec4(theme_.accent);
	c[ImGuiCol_Tab]                   = toImVec4(theme_.panel);
	c[ImGuiCol_TabHovered]            = toImVec4(theme_.border);
	c[ImGuiCol_TabSelected]           = toImVec4(theme_.panelAlt);
	c[ImGuiCol_PlotLines]             = toImVec4(theme_.accent);
	c[ImGuiCol_PlotLinesHovered]      = toImVec4(theme_.accentHover);
	c[ImGuiCol_PlotHistogram]         = toImVec4(theme_.accent);
	c[ImGuiCol_PlotHistogramHovered]  = toImVec4(theme_.accentHover);
	c[ImGuiCol_TextSelectedBg]        = toImVec4(theme_.accent, 0.35f);
	c[ImGuiCol_NavCursor]             = toImVec4(theme_.accent);
	c[ImGuiCol_TableHeaderBg]         = toImVec4(theme_.panelAlt);
	c[ImGuiCol_TableBorderStrong]     = toImVec4(theme_.border);
	c[ImGuiCol_TableBorderLight]      = toImVec4(theme_.border, 0.5f);
	c[ImGuiCol_TableRowBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	c[ImGuiCol_TableRowBgAlt]         = toImVec4(theme_.panel, 0.35f);
	c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
}

void UiLayer::setUiScale(float scale) {
	uiScale_ = std::max(0.5f, scale);
	if (!ready_) {
		return;
	}
	// Rebuild from the pristine style, then scale once. See Impl::baseStyle.
	if (impl_ && impl_->baseStyleValid) {
		ImGui::GetStyle() = impl_->baseStyle;
	}
	applyThemeToStyle();
	ImGui::GetStyle().ScaleAllSizes(uiScale_);
	// Font scaling is a separate factor in 1.92: ScaleAllSizes deliberately
	// does not touch fonts. Setting both from the same number is what keeps a
	// button's box and its label growing together.
	ImGui::GetStyle().FontScaleMain = 1.0f;
	ImGui::GetStyle().FontScaleDpi = uiScale_;
	if (fontsLoaded_) {
		// The atlas holds glyphs rasterised at the base size; 1.92 rasterises
		// on demand for other sizes, so nothing has to be rebuilt here.
		loadFonts();
	}
}

void UiLayer::setFonts(const UiFonts& fonts) {
	fonts_ = fonts;
	loadFonts();
}

void UiLayer::loadFonts() {
	if (!ready_) {
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	if (impl_->glyphRanges.empty()) {
		impl_->glyphRanges = buildGlyphRanges();
	}

	// Clear the atlas and start again: an interface font loaded at the wrong
	// scale cannot be resized in place.
	io.Fonts->Clear();
	fonts_.systemFontLoaded = false;
	fonts_.uiSource.clear();
	fonts_.monoSource.clear();

	ImFont* uiFont = nullptr;
	ImFont* monoFont = nullptr;

	const std::string uiPath = fonts_.uiPath.empty()
		? findSystemFont(false) : fonts_.uiPath;
	const std::string monoPath = fonts_.monoPath.empty()
		? findSystemFont(true) : fonts_.monoPath;

	// Font sizes are in pixels. The logical point size is scaled by the DPI
	// factor here, once: 1.92 then applies FontScaleDpi on top for the live
	// monitor scale, so the base raster stays close to what is displayed.
	const float uiPixels = fonts_.uiSizePts * uiScale_;
	const float monoPixels = fonts_.monoSizePts * uiScale_;

	// Oversampling 2x keeps the glyphs crisp when ImGui scales the raster up for
	// a dense monitor. It costs atlas area, which the ranges above keep small.
	// The name goes on the ImFontConfig, not on the returned ImFont: 1.92 keeps
	// the name in the config, so that is how PushFont("ui") finds it.
	ImFontConfig config;
	config.OversampleH = kFontOversample;
	config.OversampleV = kFontOversample;
	config.RasterizerMultiply = 1.0f;
	std::snprintf(config.Name, sizeof(config.Name), "%s", kUiFontName);

	if (!uiPath.empty() && fs::exists(uiPath)) {
		uiFont = io.Fonts->AddFontFromFileTTF(uiPath.c_str(), uiPixels, &config,
			impl_->glyphRanges.Data);
		if (uiFont != nullptr) {
			fonts_.uiSource = uiPath;
			fonts_.systemFontLoaded = true;
		}
	}
	if (uiFont == nullptr) {
		uiFont = io.Fonts->AddFontDefault(&config);
		fonts_.uiSource = "(ImGui built-in fallback)";
		LOG_WARN("UI") << "no system interface font found (looked in "
			<< systemFontDirectory() << "); using ImGui's built-in font";
	}

	if (!monoPath.empty() && fs::exists(monoPath)) {
		ImFontConfig monoConfig = config;
		std::snprintf(monoConfig.Name, sizeof(monoConfig.Name), "%s", kMonoFontName);
		monoFont = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), monoPixels,
			&monoConfig, impl_->glyphRanges.Data);
		if (monoFont != nullptr) {
			fonts_.monoSource = monoPath;
		}
	}
	if (monoFont == nullptr) {
		// No monospace font is not worth a warning: the interface is complete
		// without one, and the editor falls back to the interface font, which
		// only costs alignment in the gutter.
		monoFont = uiFont;
		fonts_.monoSource = fonts_.uiSource;
	}

	// The atlas is uploaded by the backend on the next frame, so nothing is
	// pushed to the GPU here.
	io.FontDefault = uiFont;
	fontsLoaded_ = true;
}

void UiLayer::setIniPath(std::string path) {
	if (impl_) {
		impl_->iniPath = std::move(path);
	}
	if (!ready_) {
		return;
	}
	// ImGui holds this pointer for the process's lifetime, so it must outlive
	// this call. Impl owns the string and is never reallocated after this.
	ImGui::GetIO().IniFilename = impl_->iniPath.empty()
		? nullptr : impl_->iniPath.c_str();
}

void UiLayer::installCallbacks() {
	if (!ready_ || !impl_ || impl_->window == nullptr) {
		return;
	}
	// ImGui chains whatever callbacks were already registered, so an
	// application's own handlers keep firing. Both applications install theirs
	// BEFORE this call for that reason.
	ImGui_ImplGlfw_InstallCallbacks(impl_->window);
}

void UiLayer::beginFrame(const UiFrameInfo& info) {
	if (!ready_ || !impl_) {
		return;
	}
	// The RenderDevice's own contract is that it leaves the default
	// framebuffer bound and program 0 in use at the end of a frame. Re-assert
	// the framebuffer here so ImGui can never draw into an FBO the device did
	// not mean to expose (the Player's mpv surface uses one; these two do not,
	// but the invariant is cheap and shared).
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, info.framebufferWidth, info.framebufferHeight);

	// ImGui renders with blending on. The device enables it once at
	// initialisation and never re-asserts it, so if anything ever left it off
	// every drawSolid would become opaque. Setting it here costs nothing and
	// removes that whole class of silent regression.
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(info.framebufferWidth),
		static_cast<float>(info.framebufferHeight));
	// DeltaTime must never be zero: ImGui divides by it for some animations and
	// a zero makes them jitter. A first frame has no previous frame to
	// difference, so 1/60 is the honest placeholder.
	io.DeltaTime = info.deltaSeconds > 0.0 ? static_cast<float>(info.deltaSeconds)
		: 1.0f / 60.0f;

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
	frameOpen_ = true;
}

void UiLayer::endFrame() {
	if (!ready_ || !frameOpen_) {
		return;
	}
	frameOpen_ = false;
	ImGui::Render();
	ImDrawData* data = ImGui::GetDrawData();
	if (data != nullptr) {
		ImGui_ImplOpenGL3_RenderDrawData(data);
	}
	// The backend's own RenderDrawData ends with glViewport(0,0,w,h) using the
	// DisplaySize it was given, which is the framebuffer size - so the device's
	// viewport contract still holds for the next frame. Rebinding the default
	// framebuffer makes that explicit rather than implied.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDisable(GL_SCISSOR_TEST);
}

bool UiLayer::wantsMouse() const {
	return ready_ && ImGui::GetIO().WantCaptureMouse;
}

bool UiLayer::wantsKeyboard() const {
	return ready_ && ImGui::GetIO().WantCaptureKeyboard;
}

void UiLayer::shutdown() {
	if (!ready_ && !impl_) {
		return;
	}
	ready_ = false;
	frameOpen_ = false;
	if (impl_ && impl_->window != nullptr) {
		// GL objects live here: this must run while the context is current, so
		// before the window is destroyed and before the RenderDevice is reset.
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
	}
	if (ImGui::GetCurrentContext() != nullptr) {
		ImGui::DestroyContext();
	}
	impl_.reset();
}

std::string UiLayer::describe() const {
	if (!ready_) {
		return "ImGui (not initialised)";
	}
	return std::string("ImGui ") + ImGui::GetVersion() + " / " + kGlslVersion
		+ (fonts_.systemFontLoaded ? " / system fonts" : " / built-in font");
}

} // namespace media::ui
