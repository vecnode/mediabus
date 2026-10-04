#pragma once

#include "core/AppConfig.h"

#include <string>

/// The DPI half of the text-size decision, which needs GLFW and therefore
/// cannot live in core/ with the arithmetic.
///
/// The rule is:
///
///     uiScale = sanitize(monitor content scale) overridden by mediaplayer.ini
///
/// The config override exists so an operator who still finds the text small can
/// put `uiScale = 2.5` in bin\mediaplayer.ini without a rebuild. 0 (the
/// default) leaves the DPI decision alone.
namespace media::ui {

/// The scale from the primary monitor's content scale, overridden by
/// `config.uiScale` when that is greater than zero. Never below 1.0.
float scaleForWindow(const config::Config& config);

/// The raw max(contentScaleX, contentScaleY) GLFW reported, for the log line.
/// 1.0 when no monitor answered.
float rawContentScale();

/// One log line describing the decision, e.g.
///     "1.75x (monitor 1.75, ini 0.00)".
std::string describeDecision(float uiScale);
std::string describeDecision(float uiScale, float contentScale);

} // namespace media::ui
