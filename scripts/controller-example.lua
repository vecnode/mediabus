-- controller-example.lua - a scripted session for the Controller.
--
-- Install: copy this file into bin/data/controller-scripts/ (the build does it
-- for you) and start it with
--
--     bin\media-controller-cpp.exe --script controller-example.lua
--
-- or, while the Controller is already running,
--
--     curl -X POST http://127.0.0.1:8081/api/controller/script ^
--          -d "{\"path\":\"controller-example.lua\"}"
--
-- Everything below runs inside the Controller, not the Player: each call is an
-- HTTP request to the Player's control API. The Player is never modified, so a
-- script cannot break it.
--
-- Writing a script
-- ----------------
-- Two rules matter.
--
--  1. `controller.Sleep(ms)` does not block the window. It charges a per-frame
--     budget, and the OnTick handler below is called again next frame, so the
--     script resumes where a rolling window left off. This is what lets a
--     script read as a sequence of steps instead of a state machine.
--  2. A script that never yields is aborted by a per-tick instruction budget,
--     so `while true do end` cannot freeze the bar. The error goes to the
--     Controller log and the bar keeps working.
--
-- The full API: Play, Next, Previous, Pause, Stop, SeekPercent, SetVolume,
-- SetSpeed, SetSubtitles, ShowHUD, Fullscreen, CurrentClip, Playlist, Log,
-- Sleep, OnTick.

local M = {}

-- Session state lives in this table rather than in globals, so reloading the
-- script (Ctrl+R, or POST /api/controller/reload-script) starts cleanly.
M.step = 1
M.elapsed = 0

-- Called once when the script loads. Anything that needs the Player online
-- belongs in OnTick, because the Player may not be running yet.
controller.Log("example: loaded; waiting for the player")

local function stepTitle(step)
	local titles = {
		"open the first clip and show the HUD",
		"hold a steady volume",
		"slow it down for a detail pass",
		"back to normal speed and forward",
		"done - hide the HUD and leave it playing",
	}
	return titles[step] or "finished"
end

function M.onTick()
	local clip = controller.CurrentClip()

	-- Nothing to drive until the Player answers. Do not burn the step budget
	-- waiting: just report once per second.
	if not clip.online then
		M.elapsed = M.elapsed + 1
		if M.elapsed % 60 == 0 then
			controller.Log("example: player offline, retrying")
		end
		return
	end

	if M.step == 1 then
		controller.Play(0)
		controller.SetSubtitles(true)
		controller.ShowHUD(true)
		controller.Sleep(200)
		if not controller.Sleep(0) then return end   -- budget spent, resume next frame
		M.step = 2
		M.elapsed = 0

	elseif M.step == 2 then
		controller.SetVolume(80)
		M.step = 3

	elseif M.step == 3 then
		-- Only seek when there is a timeline: a still image has none, and the
		-- Player would refuse the seek with an error we do not want to see.
		if clip.seekable and clip.duration > 0 then
			controller.SeekPercent(25)
		end
		controller.SetSpeed(0.5)
		controller.Sleep(300)
		if not controller.Sleep(0) then return end
		M.step = 4

	elseif M.step == 4 then
		controller.SetSpeed(1.0)
		controller.Next()
		M.step = 5

	elseif M.step == 5 then
		controller.ShowHUD(false)
		controller.Fullscreen(true)
		controller.Log("example: finished at clip " .. tostring(clip.index + 1)
			.. " of " .. tostring(clip.count))
		M.step = 6

	else
		-- Staying loaded is useful: the Controller keeps reporting the script
		-- as running, and a reload picks the sequence up from step 1.
		return
	end

	controller.Log("example: step " .. tostring(M.step - 1) .. " - "
		.. stepTitle(M.step - 1))
end

controller.OnTick(M.onTick)

-- Print the playlist once, to show that discovery works from a script.
for i, clip in ipairs(controller.Playlist()) do
	controller.Log(string.format("  [%d] %s (%s)", clip.index, clip.name, clip.type))
end
