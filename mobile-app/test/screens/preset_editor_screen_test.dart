import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/preset_editor_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  Future<FakeDaemonClient> pumpEditor(
    WidgetTester tester, {
    required dynamic preset,
  }) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);
    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: controller,
        rig: rigSvt,
        preset: preset,
      ),
    ));
    return fakeClient;
  }

  testWidgets('shows pinned blocks read-only and effects as switches',
      (tester) async {
    await pumpEditor(tester, preset: presetClean);

    // Amp and cab are part of the rig, shared by every preset -- shown so
    // you can see what is sounding, but with no switch to turn them off.
    expect(find.byKey(const Key('pinned-block-amp')), findsOneWidget);
    expect(find.byKey(const Key('pinned-block-cab')), findsOneWidget);
    expect(find.byKey(const Key('preset-block-switch-amp')), findsNothing);
    expect(find.byKey(const Key('preset-block-switch-cab')), findsNothing);

    // The unpinned effects are what this preset actually controls.
    expect(find.byKey(const Key('preset-block-switch-dist')), findsOneWidget);
    expect(find.byKey(const Key('preset-block-switch-reverb')), findsOneWidget);
  });

  testWidgets('resolves each switch from the preset overrides', (tester) async {
    await pumpEditor(tester, preset: presetDrive);

    final dist = tester.widget<SwitchListTile>(
      find.byKey(const Key('preset-block-switch-dist')),
    );
    final reverb = tester.widget<SwitchListTile>(
      find.byKey(const Key('preset-block-switch-reverb')),
    );

    // "Drive" overrides dist on; reverb has no override so it falls back to
    // the block's own default, which is off.
    expect(dist.value, isTrue);
    expect(reverb.value, isFalse);
  });

  testWidgets('resolves the pinned amp asset to its filename', (tester) async {
    await pumpEditor(tester, preset: presetClean);
    expect(find.text('my_amp.nam'), findsOneWidget);
  });

  testWidgets('toggling an effect and saving sends update_preset',
      (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.tap(find.byKey(const Key('preset-block-switch-dist')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final cmd = fakeClient.sentCommands
        .firstWhere((c) => c.type == 'update_preset')
        .command as UpdatePresetCommand;

    expect(cmd.rigId, 'rig-1');
    expect(cmd.presetId, 'preset-a');
    expect(cmd.blockStates!['dist']!.enabled, isTrue);
    expect(cmd.blockStates!['reverb']!.enabled, isFalse);
  });

  testWidgets('never sends block_states for pinned blocks', (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final cmd = fakeClient.sentCommands
        .firstWhere((c) => c.type == 'update_preset')
        .command as UpdatePresetCommand;

    expect(cmd.blockStates!.containsKey('amp'), isFalse);
    expect(cmd.blockStates!.containsKey('cab'), isFalse);
  });

  testWidgets('renaming and saving sends the new name', (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.enterText(
      find.byKey(const Key('preset-name-input')),
      'Clean v2',
    );
    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final cmd = fakeClient.sentCommands
        .firstWhere((c) => c.type == 'update_preset')
        .command as UpdatePresetCommand;
    expect(cmd.name, 'Clean v2');
  });
}
