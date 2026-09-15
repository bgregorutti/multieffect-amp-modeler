import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/effect_block.dart';
import 'package:mobile_app/models/footswitch_action.dart';
import 'package:mobile_app/models/preset.dart';
import 'package:mobile_app/models/ws_messages.dart';

import 'protocol_fixtures.dart';

void main() {
  group('HelloMessage', () {
    test('matches the README hello fixture', () {
      final hello =
          const HelloMessage(role: 'app', clientName: 'iphone-baptiste');
      expect(hello.toJson(), helloFixture);
    });
  });

  group('App-only commands match the daemon protocol fixtures exactly', () {
    test('create_rig', () {
      const cmd = CreateRigCommand(
        name: 'Ampeg SVT',
        chain: [
          EffectBlock(
            id: 'amp',
            type: 'nam',
            assetId: 'nam1',
            pinned: true,
          ),
          EffectBlock(
            id: 'dist',
            type: 'distortion',
            enabled: false,
            params: {'gain': 7.0},
          ),
        ],
      );
      expect(cmd.toJson(), createRigFixture);
    });

    test('create_rig with no chain omits the field (daemon default-scaffolds)',
        () {
      const cmd = CreateRigCommand(name: 'New Rig');
      expect(cmd.toJson().containsKey('chain'), isFalse);
    });

    test('update_rig (partial update omits untouched fields)', () {
      const cmd = UpdateRigCommand(rigId: 'rig1', name: 'Ampeg SVT II');
      expect(cmd.toJson(), updateRigFixture);
    });

    test('delete_rig', () {
      const cmd = DeleteRigCommand(rigId: 'rig1');
      expect(cmd.toJson(), deleteRigFixture);
    });

    test('reorder_rigs', () {
      const cmd = ReorderRigsCommand(rigIds: ['rig2', 'rig1']);
      expect(cmd.toJson(), reorderRigsFixture);
    });

    test('create_preset', () {
      const cmd = CreatePresetCommand(
        rigId: 'rig1',
        name: 'Drive',
        blockStates: {'dist': PresetBlockState(enabled: true)},
      );
      expect(cmd.toJson(), createPresetFixture);
    });

    test('update_preset (partial update omits untouched fields)', () {
      const cmd = UpdatePresetCommand(
        rigId: 'rig1',
        presetId: 'abc123',
        name: 'Drive v2',
      );
      expect(cmd.toJson(), updatePresetFixture);
    });

    test('delete_preset carries both ids -- presets live inside a rig', () {
      const cmd = DeletePresetCommand(rigId: 'rig1', presetId: 'abc123');
      expect(cmd.toJson(), deletePresetFixture);
    });

    test('select_preset with both indices', () {
      const cmd = SelectPresetCommand(rigIndex: 0, presetIndex: 2);
      expect(cmd.toJson(), selectPresetFixture);
    });

    test('select_preset omitting rig_index stays within the current rig', () {
      const cmd = SelectPresetCommand(presetIndex: 1);
      expect(cmd.toJson(), selectPresetWithinRigFixture);
      expect(cmd.toJson().containsKey('rig_index'), isFalse);
    });

    test('set_bypass', () {
      const cmd = SetBypassCommand(bypass: true);
      expect(cmd.toJson(), setBypassFixture);
    });

    test('set_footswitch_mapping (full replace, int keys -> wire strings)', () {
      const cmd = SetFootswitchMappingCommand(mapping: {
        0: NextPresetAction(),
        1: PrevPresetAction(),
        2: NextRigAction(),
        3: PrevRigAction(),
        4: ToggleBypassAction(),
      });
      expect(cmd.toJson(), setFootswitchMappingFixture);
    });

    test('register_asset', () {
      const cmd = RegisterAssetCommand(
        kind: 'nam',
        filename: 'my_amp.nam',
        storedPath: '/data/assets/abc123.nam',
        sizeBytes: 20971520,
        sha256:
            'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
      );
      expect(cmd.toJson(), registerAssetFixture);
    });
  });

  group('Server -> client envelopes parse their fixtures', () {
    test('state_snapshot', () {
      final msg = ServerMessage.fromJson(
        Map<String, dynamic>.from(stateSnapshotEnvelopeFixture),
      );
      expect(msg, isA<StateSnapshotMessage>());
      expect((msg as StateSnapshotMessage).state, isNotEmpty);
    });

    test('state_changed', () {
      final msg = ServerMessage.fromJson(
        Map<String, dynamic>.from(stateChangedEnvelopeFixture),
      );
      expect(msg, isA<StateChangedMessage>());
      expect((msg as StateChangedMessage).reason, 'select_preset');
    });

    test('command_ok', () {
      final msg = ServerMessage.fromJson(
        Map<String, dynamic>.from(commandOkFixture),
      );
      expect(msg, isA<CommandOkMessage>());
      final ok = msg as CommandOkMessage;
      expect(ok.command, 'create_rig');
      expect(ok.result, isNotEmpty);
    });

    test('error', () {
      final msg = ServerMessage.fromJson(
        Map<String, dynamic>.from(errorFixture),
      );
      expect(msg, isA<ErrorMessage>());
      final err = msg as ErrorMessage;
      expect(err.code, 'role_forbidden');
      expect(err.message, errorFixture['message']);
    });

    test('an unrecognized type comes back as UnknownMessage, not a throw', () {
      final msg = ServerMessage.fromJson({'type': 'from_the_future'});
      expect(msg, isA<UnknownMessage>());
    });
  });
}
