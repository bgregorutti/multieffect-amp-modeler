import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/effect_block.dart';
import 'package:mobile_app/models/footswitch_action.dart';
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

  group('App-only commands match README fixtures exactly', () {
    test('create_preset', () {
      const cmd = CreatePresetCommand(
        name: 'Ambient Swell',
        blocks: [
          EffectBlock(type: 'reverb', enabled: true, params: {'decay': 4.2}),
        ],
      );
      expect(cmd.toJson(), createPresetFixture);
    });

    test('update_preset (partial update omits untouched fields)', () {
      const cmd = UpdatePresetCommand(
        presetId: 'abc123',
        name: 'Ambient Swell v2',
      );
      expect(cmd.toJson(), updatePresetFixture);
    });

    test('delete_preset', () {
      const cmd = DeletePresetCommand(presetId: 'abc123');
      expect(cmd.toJson(), deletePresetFixture);
    });

    test('select_preset by id', () {
      const cmd = SelectPresetCommand.byId('abc123');
      expect(cmd.toJson(), selectPresetByIdFixture);
    });

    test('select_preset by bank_index + slot', () {
      const cmd = SelectPresetCommand.bySlot(bankIndex: 0, slot: 2);
      expect(cmd.toJson(), selectPresetBySlotFixture);
    });

    test('create_bank', () {
      const cmd = CreateBankCommand(name: 'Live Set 1', numSlots: 4);
      expect(cmd.toJson(), createBankFixture);
    });

    test('update_bank', () {
      const cmd = UpdateBankCommand(
        bankId: 'bank1',
        slots: ['abc123', null, null, null],
      );
      expect(cmd.toJson(), updateBankFixture);
    });

    test('reorder_banks', () {
      const cmd = ReorderBanksCommand(bankIds: ['bank2', 'bank1']);
      expect(cmd.toJson(), reorderBanksFixture);
    });

    test('set_bypass', () {
      const cmd = SetBypassCommand(bypass: true);
      expect(cmd.toJson(), setBypassFixture);
    });

    test('set_footswitch_mapping (full replace, int keys -> wire strings)', () {
      const cmd = SetFootswitchMappingCommand(mapping: {
        0: SelectSlotAction(slot: 0),
        1: SelectSlotAction(slot: 1),
        2: NextBankAction(),
        3: ToggleBypassAction(),
        4: TapTempoAction(),
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

  group('Server -> client envelopes parse the README fixtures', () {
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
      expect(ok.command, 'create_preset');
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
