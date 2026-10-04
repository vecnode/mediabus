-- media-player.lua - reference mpv script for media-player-cpp
--
-- Demonstrates the three capabilities the app needs from scripting:
--   1. react to playback events (property observation),
--   2. act on the player (seek, pause, track selection),
--   3. talk to the host application and receive commands from it.
--
-- Install by placing this file in <app>/bin/data/scripts/. It is loaded at
-- startup only, because mpv cannot attach a script once it is running.
--
-- Everything here runs inside mpv's own Lua engine. It has the full authority
-- of the player by design; the containment is that only files an operator puts
-- in that one directory are ever loaded.
--
-- Two API notes that cost real debugging time:
--   * mp.log() takes a LOG LEVEL first, so passing a bare message makes mpv
--     parse the text as a level name and fail. The mp.msg.* helpers take just a
--     message and are the idiomatic form.
--   * A UTF-8 BOM at the top of the file is a syntax error in Lua 5.1, so this
--     file is deliberately pure ASCII with no byte-order mark.

local mp = require 'mp'

local NAME = 'media-player'

local function log(message)
    mp.msg.info(message)
end

log('script loaded: ' .. NAME)

-- ---------------------------------------------------------------------------
-- 1. Observe playback state
-- ---------------------------------------------------------------------------

-- Watch for timestamps going backwards unexpectedly, which is how a damaged
-- file tends to present itself. A large jump is a seek and is expected.
local last_pos = 0
mp.observe_property('time-pos', 'number', function(_, value)
    if value == nil then return end
    if value < last_pos - 0.001 and (last_pos - value) < 0.5 then
        log(string.format('%s: timestamp went backwards %.3f -> %.3f',
            NAME, last_pos, value))
    end
    last_pos = value
end)

mp.observe_property('eof-reached', 'bool', function(_, value)
    if value then
        log(NAME .. ': end of file reached')
    end
end)

mp.observe_property('pause', 'bool', function(_, value)
    log(string.format('%s: %s', NAME, value and 'paused' or 'playing'))
end)

-- ---------------------------------------------------------------------------
-- 2. Act on the player when a file loads
-- ---------------------------------------------------------------------------
--
-- file-loaded fires once mpv has a demuxed file, so the properties below are
-- readable. For blocking control (holding the player before the first frame),
-- mp.register_event('on_load', ...) plus mp.commandv('hook-continue', ...) is
-- the stronger tool; this script keeps to the simpler event.

mp.register_event('file-loaded', function()
    local path = mp.get_property('path') or '?'
    local duration = mp.get_property_number('duration')
    log(string.format('%s: loaded %s (duration %s)', NAME, path,
        duration and string.format('%.2fs', duration) or 'unknown'))

    -- Example policy: skip a short leader on anything long enough that it is
    -- safe, so this cannot fight a host-issued seek on a short clip.
    if duration and duration > 10 then
        mp.commandv('seek', '0.5', 'absolute')
    end

    -- Tell the host a clip is ready, if anything is listening.
    mp.commandv('script-message', 'media-player-loaded', path)
end)

-- ---------------------------------------------------------------------------
-- 3. Commands from the host application
-- ---------------------------------------------------------------------------
--
-- The host drives this script with mpv's command:
--     script-message media-player <command> [args...]

local function handle(command, ...)
    local args = {...}
    if command == 'status' then
        local pos = mp.get_property_number('time-pos') or 0
        local dur = mp.get_property_number('duration') or 0
        log(string.format('%s: status %.2f/%.2f', NAME, pos, dur))
        mp.commandv('script-message', 'media-player-status',
            string.format('%.3f', pos), string.format('%.3f', dur))
    elseif command == 'chapter' then
        local index = tonumber(args[1]) or 1
        mp.commandv('set', 'chapter', tostring(index))
        log(string.format('%s: jumped to chapter %d', NAME, index))
    elseif command == 'rate' then
        local rate = tonumber(args[1]) or 1.0
        mp.set_property_number('speed', rate)
        log(string.format('%s: speed set to %.2f', NAME, rate))
    else
        log(string.format('%s: unknown command %s', NAME, tostring(command)))
    end
end

mp.register_script_message('media-player', handle)

log('script ready: ' .. NAME)
