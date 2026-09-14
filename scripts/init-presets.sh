echo "Guitar Rig"
control-daemon/.venv/bin/python3 scripts/load_test_preset.py \
  --name "Cab Orange 2x12" \
  --rig "Guitar Rig" \
  --ir "/Users/bgregorutti/Documents/Musique/VST-NAM/Shift_Line_Orange_JR212_IR_Pack/01_Orange_JR212_cab50_L19_cone_e906.wav" \
  --no-delay --delay-ms 0 --delay-feedback 0 --delay-mix 0

control-daemon/.venv/bin/python3 scripts/load_test_preset.py \
  --name "Head Blackstar S1-200" \
  --rig "Guitar Rig" \
  --nam "/Users/bgregorutti/Documents/Musique/VST-NAM/Blackstar S1 200 Amp/Blacksar S1 200_CRUNCH_Amp.nam" \
  --no-delay --delay-ms 0 --delay-feedback 0 --delay-mix 0

echo "Bass Rig"
control-daemon/.venv/bin/python3 scripts/load_test_preset.py \
  --name "Head Orange Bass Terror" \
  --rig "Bass Rig" \
  --nam "/Users/bgregorutti/Documents/Musique/VST-NAM/Orange Terror Bass 500/Terror Bass V4_G8 - T5_M5_B7 - Clean.nam" \
  --no-delay --delay-ms 0 --delay-feedback 0 --delay-mix 0

control-daemon/.venv/bin/python3 scripts/load_test_preset.py \
  --name "Cab Orange Bass 2x12" \
  --rig "Bass Rig" \
  --ir "/Users/bgregorutti/Documents/Musique/VST-NAM/Shift_Line_Bass_IR_Pack/09_Orange_PPC212_bass_edition_by_Shift_Line.wav" \
  --no-delay --delay-ms 0 --delay-feedback 0 --delay-mix 0
