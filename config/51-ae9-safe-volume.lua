-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (c) 2026 3xHelix
-- Installed by the packages (WirePlumber 0.4's main.lua.d): the WirePlumber
-- 0.4 twin of 50-ae9-safe-volume.conf, for Ubuntu 24.04 / Mint 22, Debian 12
-- and openSUSE Leap. WirePlumber 0.5 never loads this file (it reads the
-- .conf instead), and 0.4 ignores the .conf.
--
-- With the card's hardware volume, PipeWire's output volume IS the head
-- unit's volume: 100% = 0.0 dB, the DAC wide open. The driver starts the
-- head unit at -36.0 dB, but WirePlumber then sets the volume it saved for
-- the output, or, for an output it has none for, its default (-24 dB).
--
-- WirePlumber 0.4 can't set that default for one card, only for all of
-- them: with this file, ANY output WirePlumber has no saved volume for
-- starts at -36 dB instead of -24 dB (quieter, never louder). Volumes you
-- set are still saved and restored as usual. Don't want it? Delete it, or
-- override it with your own ~/.config/wireplumber/main.lua.d/51-ae9-safe-volume.lua.
--
-- Volumes here are linear: -36 dB = 10^(-36/20) = 0.0158
-- (WirePlumber 0.4's own default is 0.4^3 = 0.064, -24 dB).
if device_defaults ~= nil and device_defaults.properties ~= nil then
  device_defaults.properties["default-volume"] = 0.0158
end
