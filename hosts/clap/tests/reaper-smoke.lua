-- SPDX-License-Identifier: MIT
-- REAPER smoke test for the Squatch plugins, run inside REAPER (Actions > Show action list >
-- New action > Load ReaScript..., or `REAPER -nonewinst hosts/clap/tests/reaper-smoke.lua`).
-- In a new project tab: one track per installed Squatch plugin (CLAP, VST3 and AU), each fed a
-- generated A2 sine 13 cents sharp, played for 4 s with the master at -inf (nothing is heard).
-- Prints, per plugin: that REAPER found and instantiated it, the track's peak, and every
-- parameter as REAPER formats it (the tuner's Note should read A, Cents about +13.0).
-- The tab is left open to look at; close it without saving.
local tmp = (os.getenv("TMPDIR") or "/tmp/"):gsub("/?$", "/")
local wav = tmp .. "squatch-smoke-a2.wav"

local function writeSine(path)
  local rate, n, hz = 48000, 48000 * 6, 110 * 2 ^ (13 / 1200)
  local f = assert(io.open(path, "wb"))
  f:write("RIFF", string.pack("<I4", 36 + 4 * n), "WAVEfmt ", string.pack("<I4I2I2I4I4I2I2", 16, 3, 1, rate, rate * 4, 4, 32))
  f:write("data", string.pack("<I4", 4 * n))
  for i = 0, n - 1 do f:write(string.pack("<f", 0.3 * math.sin(2 * math.pi * hz * i / rate))) end
  f:close()
end

local function squatchPlugins()
  local names, i = {}, 0
  while true do
    local ok, name = reaper.EnumInstalledFX(i)
    if not ok then return names end
    if name:find("Squatch") then names[#names + 1] = name end
    i = i + 1
  end
end

local function formatted(tr, fx)
  local s = {}
  for p = 0, reaper.TrackFX_GetNumParams(tr, fx) - 1 do
    local _, name = reaper.TrackFX_GetParamName(tr, fx, p)
    local _, value = reaper.TrackFX_GetFormattedParamValue(tr, fx, p)
    s[#s + 1] = name .. "=" .. value
  end
  return table.concat(s, "; ")
end

local function say(s) reaper.ShowConsoleMsg(s .. "\n") end

writeSine(wav)
reaper.Main_OnCommand(40859, 0) -- New project tab
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "D_VOL", 0)
local tracks, names = {}, squatchPlugins()
say(("REAPER %s: %d Squatch plugins installed"):format(reaper.GetAppVersion(), #names))
for k, name in ipairs(names) do
  reaper.InsertTrackAtIndex(k - 1, false)
  local tr = reaper.GetTrack(0, k - 1)
  reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", name, true)
  reaper.SetOnlyTrackSelected(tr)
  reaper.SetEditCurPos(0, false, false)
  reaper.InsertMedia(wav, 0)
  tracks[#tracks + 1] = { tr = tr, fx = reaper.TrackFX_AddByName(tr, name, false, -1), name = name }
end
reaper.SetEditCurPos(0, false, false)
reaper.OnPlayButton()
local start = reaper.time_precise()
local function wait()
  if reaper.time_precise() - start < 4 then return reaper.defer(wait) end
  for _, t in ipairs(tracks) do
    say(("%s %s | peak %.3f | %s"):format(t.fx >= 0 and "ok  " or "FAIL", t.name, reaper.Track_GetPeakInfo(t.tr, 0),
      t.fx >= 0 and formatted(t.tr, t.fx) or "not instantiated"))
  end
  reaper.OnStopButton()
end
reaper.defer(wait)
