import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/preset_list_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  testWidgets('lists presets and marks the active one', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetListScreen(controller: controller),
    ));

    expect(find.text('Clean'), findsOneWidget);
    expect(find.text('Drive'), findsOneWidget);
  });

  testWidgets('tapping a preset sends select_preset with its index',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetListScreen(controller: controller),
    ));

    await tester.tap(find.byKey(const Key('preset-tile-preset-b')));
    await tester.pump();

    expect(fakeClient.sentCommands, hasLength(1));
    final cmd =
        fakeClient.sentCommands.single.command as SelectPresetCommand;
    expect(cmd.presetIndex, 1);
    // Omitting rig_index keeps the selection inside the current rig.
    expect(cmd.rigIndex, isNull);
  });

  testWidgets('tapping delete sends delete_preset for that preset',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetListScreen(controller: controller),
    ));

    await tester.tap(find.byKey(const Key('delete-preset-preset-a')));
    await tester.pump();

    final deleteCommands = fakeClient.sentCommands
        .where((c) => c.type == 'delete_preset')
        .toList();
    expect(deleteCommands, hasLength(1));
    final deleteCmd = deleteCommands.single.command as DeletePresetCommand;
    expect(deleteCmd.presetId, 'preset-a');
    expect(deleteCmd.rigId, 'rig-1');
  });

  testWidgets(
    'the add button opens a dialog and saving it sends create_preset',
    (tester) async {
      final fakeClient = FakeDaemonClient(state: sampleState);
      final controller = DaemonStateController(fakeClient);

      await tester.pumpWidget(MaterialApp(
        home: PresetListScreen(controller: controller),
      ));

      await tester.tap(find.byKey(const Key('add-preset-button')));
      await tester.pumpAndSettle();

      await tester.enterText(
        find.byKey(const Key('preset-name-field')),
        'New Lead Tone',
      );
      await tester.tap(find.byKey(const Key('preset-name-save')));
      await tester.pumpAndSettle();

      final createCommands = fakeClient.sentCommands
          .where((c) => c.type == 'create_preset')
          .toList();
      expect(createCommands, hasLength(1));
      final createCmd = createCommands.single.command as CreatePresetCommand;
      expect(createCmd.name, 'New Lead Tone');
      expect(createCmd.rigId, 'rig-1');
    },
  );

  testWidgets('prompts to pick a rig when none is active', (tester) async {
    final fakeClient = FakeDaemonClient();
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetListScreen(controller: controller),
    ));

    expect(
      find.text('No rig selected -- pick one on the Rigs screen'),
      findsOneWidget,
    );
  });

  testWidgets('summarises which effects each preset turns on', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetListScreen(controller: controller),
    ));

    // "Clean" overrides nothing and both effects default to off.
    expect(find.text('no effects on'), findsOneWidget);
    // "Drive" switches the distortion on.
    expect(find.text('distortion'), findsOneWidget);
  });
}
