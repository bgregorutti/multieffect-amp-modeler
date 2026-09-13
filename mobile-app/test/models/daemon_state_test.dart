import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/asset.dart';
import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/footswitch_action.dart';

import 'protocol_fixtures.dart';

void main() {
  group('DaemonState', () {
    test('parses the full daemon state fixture field-by-field', () {
      final state = DaemonState.fromJson(fullDaemonStateFixture);

      expect(state.version, 1);
      expect(state.presets.keys, ['abc123']);
      expect(state.presets['abc123']!.name, 'Ambient Swell');

      expect(state.banks, hasLength(1));
      expect(state.banks.first.id, 'bank1');

      expect(state.assets.keys, ['nam1']);
      expect(state.assets['nam1']!.kind, AssetKind.nam);

      // Wire keys are JSON-object strings ("0", "1", ...); DaemonState
      // exposes them as int keys for convenient Dart use.
      expect(state.footswitchMapping.keys.toSet(), {0, 1, 2, 3, 4, 5});
      expect(state.footswitchMapping[0], isA<SelectSlotAction>());
      expect(state.footswitchMapping[2], isA<NextBankAction>());
      expect(state.footswitchMapping[3], isA<PrevBankAction>());

      expect(state.activeBankIndex, 0);
      expect(state.activeSlot, 0);
      expect(state.activePresetId, 'abc123');
      expect(state.bypass, false);
      expect(state.tempoBpm, 120.0);

      expect(state.activePreset?.id, 'abc123');
    });

    test('round-trips through toJson/fromJson, including int->string keys', () {
      final original = DaemonState.fromJson(fullDaemonStateFixture);
      final roundTripped = DaemonState.fromJson(original.toJson());

      expect(roundTripped.presets.keys, original.presets.keys);
      expect(roundTripped.banks.length, original.banks.length);
      expect(roundTripped.assets.keys, original.assets.keys);
      expect(
        roundTripped.footswitchMapping.keys.toSet(),
        original.footswitchMapping.keys.toSet(),
      );
      expect(roundTripped.activePresetId, original.activePresetId);
      expect(roundTripped.tempoBpm, original.tempoBpm);
    });

    test('DaemonState.empty has no active preset and default fields', () {
      const state = DaemonState.empty;
      expect(state.activePreset, isNull);
      expect(state.presets, isEmpty);
      expect(state.banks, isEmpty);
      expect(state.bypass, false);
      expect(state.tempoBpm, isNull);
    });

    test(
      'handles a placeholder-shaped snapshot state (envelope fixture) '
      'gracefully by defaulting missing fields',
      () {
        // The README's state_snapshot/state_changed example envelopes use a
        // placeholder `"...": "..."` for `state` rather than a real
        // DaemonState (the full shape is in models.py) -- parsing that
        // shouldn't throw, it should just come back as all-defaults.
        final state = DaemonState.fromJson(
          Map<String, dynamic>.from(
            stateSnapshotEnvelopeFixture['state'] as Map,
          ),
        );
        expect(state.presets, isEmpty);
        expect(state.activePresetId, isNull);
      },
    );
  });
}
