# Audio

Card `sm7250-bolero-snd-card` (ADSP, q6 ASM/AFE). The ALSA UCM profile is
in `device-google-bramble/ucm/`, built from the stock
`mixer_paths_bolero_snd.xml`. alsa-lib finds it by driver name, which the
kernel cuts to `sm7250-bolero-s`.

- Speakers: two Cirrus CS35L41 amps on QUIN TDM. Stock Android runs the
  Cirrus speaker-protection DSP firmware, which needs per-device
  calibration. This port bypasses the amp DSP (`PCM Source` = `ASP`) and
  uses the stock amp gain and volume (0 dB).
- Main microphone: Realtek RT5514 on PRI TDM. Capture must be two
  channels; mono capture returns only zeros.
- Headphones and headset mic: WCD938x. Plug detection works through MBHC.
  Patch 0004 adds ALSA jack controls ("Headphone Jack", "Headset Mic Jack")
  so PulseAudio can switch outputs on plug-in. PulseAudio reads them
  through alsa-lib's simple mixer, which reads every boolean, integer and
  enumerated control while it loads and gives up on the whole card at the
  first failed read. 57 q6 controls fail (write-only, or readable only
  while a DSP session runs), so patch 0005 adds a per-card flag that makes
  failed control reads return zeros, set by the machine driver (`kona.c`).
  Check: `amixer -c0 scontrols` lists controls instead of printing
  "Mixer load error".
- MultiMedia1 must stay routed to a backend while PulseAudio probes the
  card, or the DSP rejects `hw_params` and PulseAudio drops the card. The
  UCM verb routes it.
- PipeWire backend: `device-google-bramble-pipewire` (installed together
  with PipeWire) ships a WirePlumber config (`51-bramble.conf`) that
  disables PipeWire's extra compress-offload device for the card and the
  camera monitors. WirePlumber stopped handling events after probing the
  downstream V4L2 nodes, so the audio nodes were never linked and nothing
  played.
  Speaker and headphones are separate ACP profiles (they share PCM 0);
  WirePlumber picks the best available profile on plug and unplug. A
  profile chosen by hand is saved in `~/.local/state/wireplumber/
  default-profile` and then wins over the automatic choice; delete that
  file to get automatic switching back.
- Speaker loudness (PipeWire): on Android the bypassed protection DSP
  also made the speaker louder. `device-google-bramble-pipewire` adds a
  filter-chain that WirePlumber puts in front of the speaker sink only
  (smart filter): +6 dB, then the LSP look-ahead limiter holds the peaks
  at -1 dBFS. LSP's "Bypass" control must be 0 (0 means processing) and
  its "Automatic level regulation" off, or the output stays near -9 dBFS.
