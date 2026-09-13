import 'package:mobile_app/models/asset.dart';
import 'package:mobile_app/models/bank.dart';
import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/effect_block.dart';
import 'package:mobile_app/models/footswitch_action.dart';
import 'package:mobile_app/models/preset.dart';

const presetA = Preset(
  id: 'preset-a',
  name: 'Ambient Swell',
  blocks: [
    EffectBlock(type: 'reverb', enabled: true, params: {'decay': 4.2}),
  ],
  namAssetId: 'nam-1',
  createdAt: 0,
  updatedAt: 0,
);

const presetB = Preset(
  id: 'preset-b',
  name: 'Crunch Rhythm',
  blocks: [],
  createdAt: 0,
  updatedAt: 0,
);

const bankOne = Bank(id: 'bank-1', name: 'Live Set 1', slots: ['preset-a', null]);

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
  presets: const {'preset-a': presetA, 'preset-b': presetB},
  banks: const [bankOne],
  assets: const {'nam-1': namAsset, 'ir-1': irAsset},
  footswitchMapping: const {
    0: SelectSlotAction(slot: 0),
    1: NextBankAction(),
  },
  activePresetId: 'preset-a',
  bypass: false,
  tempoBpm: 120.0,
);
