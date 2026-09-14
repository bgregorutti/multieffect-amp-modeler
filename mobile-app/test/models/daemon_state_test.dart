import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/footswitch_action.dart';

import 'protocol_fixtures.dart';

void main() {
  test('parses the full daemon state fixture', () {
    final state =
        DaemonState.fromJson(Map<String, dynamic>.from(fullDaemonStateFixture));

    expect(state.version, 2);
    expect(state.rigs, hasLength(1));

    final rig = state.rigs.single;
    expect(rig.id, 'rig1');
    expect(rig.name, 'Ampeg SVT');
    expect(rig.chain, hasLength(2));
    expect(rig.presets, hasLength(1));

    expect(state.assets['nam1']!.filename, 'my_amp.nam');
    expect(state.footswitchMapping[0], const NextPresetAction());
    expect(state.footswitchMapping[2], const NextRigAction());
    expect(state.footswitchMapping[6], const SelectPresetAction(index: 2));
    expect(state.bypass, isFalse);
    expect(state.tempoBpm, 120.0);
  });

  test('separates pinned backline from switchable effects', () {
    final state =
        DaemonState.fromJson(Map<String, dynamic>.from(fullDaemonStateFixture));
    final rig = state.rigs.single;

    expect(rig.pinnedBlocks.map((b) => b.id), ['amp']);
    expect(rig.switchableBlocks.map((b) => b.id), ['reverb']);
  });

  test('preserves chain order, which is the signal path', () {
    final state =
        DaemonState.fromJson(Map<String, dynamic>.from(fullDaemonStateFixture));
    expect(state.rigs.single.chain.map((b) => b.id), ['amp', 'reverb']);
  });

  test('resolves the active rig and preset from the indices', () {
    final state =
        DaemonState.fromJson(Map<String, dynamic>.from(fullDaemonStateFixture));

    expect(state.activeRig!.id, 'rig1');
    expect(state.activePreset!.id, 'abc123');
    expect(state.activeRigId, 'rig1');
    expect(state.activePresetId, 'abc123');
  });

  test('out-of-range indices resolve to null instead of throwing', () {
    // The daemon clamps, but a stale snapshot mid-delete could still arrive
    // with an index past the end -- the UI must not crash on it.
    final json = Map<String, dynamic>.from(fullDaemonStateFixture);
    json['active_rig_index'] = 9;
    final state = DaemonState.fromJson(json);

    expect(state.activeRig, isNull);
    expect(state.activePreset, isNull);
  });

  test('an empty state parses and resolves to nothing active', () {
    final state = DaemonState.fromJson({'version': 2});

    expect(state.rigs, isEmpty);
    expect(state.activeRig, isNull);
    expect(state.activePreset, isNull);
    expect(DaemonState.empty.activeRig, isNull);
  });

  test('rigById finds a rig by id', () {
    final state =
        DaemonState.fromJson(Map<String, dynamic>.from(fullDaemonStateFixture));

    expect(state.rigById('rig1')!.name, 'Ampeg SVT');
    expect(state.rigById('nope'), isNull);
  });

  test('round-trips the stored schema, omitting the derived active ids', () {
    final state =
        DaemonState.fromJson(Map<String, dynamic>.from(fullDaemonStateFixture));
    final json = state.toJson();

    expect(json['version'], 2);
    expect(json['active_rig_index'], 0);
    expect(json['active_preset_index'], 0);
    // Derived by the daemon's state_view, not part of what it stores.
    expect(json.containsKey('active_rig_id'), isFalse);
    expect(json.containsKey('active_preset_id'), isFalse);

    final reparsed = DaemonState.fromJson(json);
    expect(reparsed.rigs.single.chain.map((b) => b.id),
        state.rigs.single.chain.map((b) => b.id));
    expect(reparsed.footswitchMapping, state.footswitchMapping);
  });

  test('footswitch mapping keys convert between string wire and int', () {
    final state =
        DaemonState.fromJson(Map<String, dynamic>.from(fullDaemonStateFixture));

    expect(state.footswitchMapping.keys, everyElement(isA<int>()));
    expect(state.toJson()['footswitch_mapping'].keys,
        everyElement(isA<String>()));
  });
}
