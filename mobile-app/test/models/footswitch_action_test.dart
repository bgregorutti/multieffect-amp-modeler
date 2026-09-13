import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/footswitch_action.dart';

import 'protocol_fixtures.dart';

void main() {
  group('FootswitchAction', () {
    test('parses every action in the set_footswitch_mapping README fixture', () {
      final mapping =
          setFootswitchMappingFixture['mapping'] as Map<String, dynamic>;

      final zero = FootswitchAction.fromJson(
        mapping['0'] as Map<String, dynamic>,
      );
      expect(zero, isA<SelectSlotAction>());
      expect((zero as SelectSlotAction).slot, 0);

      final one = FootswitchAction.fromJson(
        mapping['1'] as Map<String, dynamic>,
      );
      expect((one as SelectSlotAction).slot, 1);

      expect(
        FootswitchAction.fromJson(mapping['2'] as Map<String, dynamic>),
        isA<NextBankAction>(),
      );
      expect(
        FootswitchAction.fromJson(mapping['3'] as Map<String, dynamic>),
        isA<ToggleBypassAction>(),
      );
      expect(
        FootswitchAction.fromJson(mapping['4'] as Map<String, dynamic>),
        isA<TapTempoAction>(),
      );
    });

    test('parses prev_bank (from the full daemon state fixture)', () {
      final mapping =
          fullDaemonStateFixture['footswitch_mapping'] as Map<String, dynamic>;
      final action = FootswitchAction.fromJson(
        mapping['3'] as Map<String, dynamic>,
      );
      expect(action, isA<PrevBankAction>());
    });

    test('round-trips every action kind through toJson/fromJson', () {
      const actions = <FootswitchAction>[
        SelectSlotAction(slot: 7),
        NextBankAction(),
        PrevBankAction(),
        ToggleBypassAction(),
        TapTempoAction(),
      ];

      for (final action in actions) {
        final roundTripped = FootswitchAction.fromJson(action.toJson());
        expect(roundTripped, equals(action));
        expect(roundTripped.wireType, action.wireType);
      }
    });

    test('rejects an unknown action type', () {
      expect(
        () => FootswitchAction.fromJson({'type': 'levitate'}),
        throwsArgumentError,
      );
    });

    test('kFootswitchActionKinds lists exactly the five documented kinds', () {
      expect(kFootswitchActionKinds, [
        'select_slot',
        'next_bank',
        'prev_bank',
        'toggle_bypass',
        'tap_tempo',
      ]);
    });
  });
}
