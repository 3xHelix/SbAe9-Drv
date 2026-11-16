SbAe9-Drv 0.12.11-rc1 - release candidate, for ASMedia-bridge testers first
==========================================================================

This is the 0.12.11 driver before its release: what the author runs daily,
tested on an IDT-bridge card. We'd love to see it on an ASMedia-bridge card
(ASM1083/1085, PCI 1b21:1080) before it ships. The full README and CHANGELOG
are in this folder (CHANGELOG: "0.12.11 (unreleased, DRAFT)").

What's new since 0.12.10
  - DSP mode (Direct Mode off) plays at full level. It was 6 dB quieter than
    Direct Mode and only half of the audio got through: the DSP feeds the
    DACs on two lanes per channel and the driver only connected one. Now
    every lane is connected, in the router chip's own slots for them.
    !! DSP MODE IS NOW 6 dB LOUDER at the same volume, headphones too:
    turn the volume down before you first switch Direct Mode off. !!
    Bass Redirect now reaches the sub every time.
  - Fixed: the mic could sound like a chipmunk (or DSP mode go silent) after
    playing something at 96/192 kHz and then switching Direct Mode off.
  - Fixed: unloading the driver (modprobe -r) could crash the kernel if the
    card stopped answering during the unload.
  - Safer DMA when a stream won't stop, and a hardened command ring (a bad
    read through a PCIe bridge is never used as a position).
  Pairs with SbAe9-SBC (current) and SbAe9-AudioMon v0.4.7.

Install (from this folder)
  Use ONE install method per machine (see README "One install method").
  - Installed from source / DKMS before:
        sudo scripts/dkms-install.sh
    It replaces the old version.
  - Installed a package (COPR akmod, PPA, .deb, AUR): remove the package
    first, then  sudo scripts/dkms-install.sh  . To go back:
    sudo scripts/dkms-install.sh --remove  and reinstall the package.
  Then shut down, PSU off ~30 s, boot. Check:
        cat /sys/module/snd_hda_codec_creative_ae9/version    -> 0.12.11-rc1

What helps us most (ASMedia cards especially)
  1. Use it normally for a day or two: DSP mode / SBX, Direct Mode, a few
     sample-rate changes (96/192 kHz), headphones/speakers, Discord or any
     app using the mic, What U Hear if you use it.
  2. The command-ring counters, after a busy session (e.g. toggling
     SBX/effects a lot with something recording):
        sudo grep -E 'RIRB|timeouts' /sys/kernel/debug/creative_ae9/state
     "verb timeouts", "never landed", "other codec" and "unreadable write
     pointer" should stay 0. "landed late ... (longest wait N us)" is what
     we most want from an ASMedia card: how late the card's answers land
     behind your bridge (ours: up to 8 us).
  3. One full dump while playing in DSP mode:
        sudo scripts/ae9-full-dump.sh     (or ae9-full-dump if installed)
     The line "router stream 0x18: ..." shows whether your card's router
     chip set up the DSP routes by itself (lanes -> 40 41 50 ...) or left
     them at ff (ours does: we fill them in).
  4. At least one WARM reboot (no PSU off), one driver reload
     (ae9-reload) and one suspend/resume if you use it: sound in Direct
     Mode AND DSP mode afterwards?
  5. Anything odd: send  journalctl -k -b | grep creative-ae9  and a full dump.
Thank you!
From sbae9 5dc5076, 2026-11-15.
