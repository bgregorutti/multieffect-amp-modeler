
echo "Bass Rig"
control-daemon/.venv/bin/python3 scripts/load_test_preset.py \
  --name "Head Orange Bass Terror" \
  --rig "Bass Rig" \
  --nam "/Users/bgregorutti/Documents/Musique/VST-NAM/Orange Terror Bass 500/Terror Bass V4_G8 - T5_M5_B7 - Clean.nam" \
  --ir "/Users/bgregorutti/Documents/Musique/VST-NAM/Shift_Line_Bass_IR_Pack/09_Orange_PPC212_bass_edition_by_Shift_Line.wav" \
  --no-delay --delay-ms 0 --delay-feedback 0 --delay-mix 0

echo "Guitar Rig"
# Head + cab pinned at the rig level (unaffected by preset switching);
# Boost/Delay are native switchable blocks toggled per preset -- see
# scripts/build_guitar_rig.py's module docstring for why this replaces
# the old one-load_test_preset.py-call-per-preset approach.
control-daemon/.venv/bin/python3 scripts/build_guitar_rig.py \
  --rig "Guitar Rig" \
  --nam "/Users/bgregorutti/Documents/Musique/VST-NAM/Blackstar S1 200 Amp/Blacksar S1 200_CRUNCH_Amp.nam" \
  --ir "/Users/bgregorutti/Documents/Musique/VST-NAM/Shift_Line_Orange_JR212_IR_Pack/01_Orange_JR212_cab50_L19_cone_e906.wav"
