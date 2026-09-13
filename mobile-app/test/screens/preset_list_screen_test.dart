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

    expect(find.text('Ambient Swell'), findsOneWidget);
    expect(find.text('Crunch Rhythm'), findsOneWidget);
  });

  testWidgets('tapping a preset sends select_preset with its id',
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
    expect(cmd.presetId, 'preset-b');
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
    expect(
      (deleteCommands.single.command as DeletePresetCommand).presetId,
      'preset-a',
    );
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
      expect(
        (createCommands.single.command as CreatePresetCommand).name,
        'New Lead Tone',
      );
    },
  );

  testWidgets('shows an empty message when there are no presets',
      (tester) async {
    final fakeClient = FakeDaemonClient();
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetListScreen(controller: controller),
    ));

    expect(find.text('No presets yet'), findsOneWidget);
  });
}
