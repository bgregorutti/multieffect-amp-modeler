import 'package:mobile_app/models/asset.dart';
import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/effect_block.dart';
import 'package:mobile_app/models/footswitch_action.dart';
import 'package:mobile_app/models/preset.dart';
import 'package:mobile_app/models/rig.dart';

/// A representative bass rig: pinned amp + cab that every preset shares,
/// plus two switchable effects.
const svtChain = [
  EffectBlock(id: 'amp', type: 'nam', assetId: 'nam-1', pinned: true),
  EffectBlock(id: 'cab', type: 'ir', assetId: 'ir-1', pinned: true),
  EffectBlock(id: 'dist', type: 'distortion', enabled: false),
  EffectBlock(id: 'reverb', type: 'reverb', enabled: false, params: {'decay': 4.2}),
];

const presetClean = Preset(
  id: 'preset-a',
  name: 'Clean',
  createdAt: 0,
  updatedAt: 0,
);

const presetDrive = Preset(
  id: 'preset-b',
  name: 'Drive',
  blockStates: {'dist': PresetBlockState(enabled: true)},
  createdAt: 0,
  updatedAt: 0,
);

const rigSvt = Rig(
  id: 'rig-1',
  name: 'Ampeg SVT',
  chain: svtChain,
  presets: [presetClean, presetDrive],
);

const rigOrange = Rig(
  id: 'rig-2',
  name: 'Orange Terror',
  chain: [
    EffectBlock(id: 'amp2', type: 'nam', pinned: true),
    EffectBlock(id: 'fuzz', type: 'fuzz', enabled: false),
  ],
  presets: [
    Preset(id: 'preset-c', name: 'Fuzz Out', createdAt: 0, updatedAt: 0),
  ],
);

const namAsset = Asset(
  id: 'nam-1',
  kind: AssetKind.nam,
  filename: 'my_amp.nam',
  storedPath: '/data/assets/x.nam',
  sizeBytes: 2048,
  uploadedAt: 0,
);

const irAsset = Asset(
  id: 'ir-1',
  kind: AssetKind.ir,
  filename: 'cab.wav',
  storedPath: '/data/assets/y.wav',
  sizeBytes: 4096,
  uploadedAt: 0,
);

final sampleState = DaemonState(
  rigs: const [rigSvt, rigOrange],
  assets: const {'nam-1': namAsset, 'ir-1': irAsset},
  footswitchMapping: const {
    0: NextPresetAction(),
    1: PrevPresetAction(),
    2: NextRigAction(),
    3: PrevRigAction(),
  },
  activeRigIndex: 0,
  activePresetIndex: 0,
  activeRigId: 'rig-1',
  activePresetId: 'preset-a',
  bypass: false,
  tempoBpm: 120.0,
);
