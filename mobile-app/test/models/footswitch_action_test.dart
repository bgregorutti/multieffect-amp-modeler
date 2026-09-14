import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/footswitch_action.dart';

void main() {
  test('parses every action type the daemon can send', () {
    expect(
      FootswitchAction.fromJson({'type': 'next_rig'}),
      const NextRigAction(),
    );
    expect(
      FootswitchAction.fromJson({'type': 'prev_rig'}),
      const PrevRigAction(),
    );
    expect(
      FootswitchAction.fromJson({'type': 'next_preset'}),
      const NextPresetAction(),
    );
    expect(
      FootswitchAction.fromJson({'type': 'prev_preset'}),
      const PrevPresetAction(),
    );
    expect(
      FootswitchAction.fromJson({'type': 'select_preset', 'index': 2}),
      const SelectPresetAction(index: 2),
    );
    expect(
      FootswitchAction.fromJson({'type': 'toggle_bypass'}),
      const ToggleBypassAction(),
    );
    expect(
      FootswitchAction.fromJson({'type': 'tap_tempo'}),
      const TapTempoAction(),
    );
  });

  test('round-trips each action back to its wire shape', () {
    expect(const NextRigAction().toJson(), {'type': 'next_rig'});
    expect(const PrevRigAction().toJson(), {'type': 'prev_rig'});
    expect(const NextPresetAction().toJson(), {'type': 'next_preset'});
    expect(const PrevPresetAction().toJson(), {'type': 'prev_preset'});
    expect(const SelectPresetAction(index: 3).toJson(),
        {'type': 'select_preset', 'index': 3});
    expect(const ToggleBypassAction().toJson(), {'type': 'toggle_bypass'});
    expect(const TapTempoAction().toJson(), {'type': 'tap_tempo'});
  });

  test('rig and preset stepping are distinct actions, not aliases', () {
    // They look similar but mean very different things: rig stepping
    // reloads the amp and cab, preset stepping does not.
    expect(const NextRigAction() == const NextPresetAction(), isFalse);
    expect(const PrevRigAction() == const PrevPresetAction(), isFalse);
  });

  test('select_preset compares by index', () {
    expect(const SelectPresetAction(index: 1),
        equals(const SelectPresetAction(index: 1)));
    expect(const SelectPresetAction(index: 1),
        isNot(equals(const SelectPresetAction(index: 2))));
  });

  test('an unknown action type throws rather than being silently dropped', () {
    expect(
      () => FootswitchAction.fromJson({'type': 'not_a_real_action'}),
      throwsArgumentError,
    );
  });

  test('kFootswitchActionKinds covers every parseable type', () {
    for (final kind in kFootswitchActionKinds) {
      final json = <String, dynamic>{'type': kind};
      if (kind == 'select_preset') json['index'] = 0;
      expect(FootswitchAction.fromJson(json).wireType, kind);
    }
  });
}
