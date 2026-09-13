import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/preset_editor_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  testWidgets('renders the preset name and its blocks', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(controller: controller, preset: presetA),
    ));

    final nameField =
        tester.widget<TextField>(find.byKey(const Key('preset-name-input')));
    expect(nameField.controller!.text, 'Ambient Swell');

    expect(find.byKey(const Key('block-type-field-0')), findsOneWidget);
    final typeField = tester
        .widget<TextFormField>(find.byKey(const Key('block-type-field-0')));
    expect(typeField.initialValue, 'reverb');
  });

  testWidgets('renaming and saving sends update_preset with the new name',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(controller: controller, preset: presetA),
    ));

    await tester.enterText(
      find.byKey(const Key('preset-name-input')),
      'Ambient Swell v2',
    );
    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final updateCommands = fakeClient.sentCommands
        .where((c) => c.type == 'update_preset')
        .toList();
    expect(updateCommands, hasLength(1));
    final cmd = updateCommands.single.command as UpdatePresetCommand;
    expect(cmd.presetId, 'preset-a');
    expect(cmd.name, 'Ambient Swell v2');
    // blocks/asset ids should be sent too (a full commit of the draft).
    expect(cmd.blocks, isNotNull);
    expect(cmd.blocks!.single.type, 'reverb');
  });

  testWidgets('adding a block appends a generic new_block entry',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(controller: controller, preset: presetA),
    ));

    expect(find.byKey(const Key('block-card-0')), findsOneWidget);
    expect(find.byKey(const Key('block-card-1')), findsNothing);

    await tester.tap(find.byKey(const Key('add-block-button')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('block-card-1')), findsOneWidget);
    final newTypeField = tester
        .widget<TextFormField>(find.byKey(const Key('block-type-field-1')));
    expect(newTypeField.initialValue, 'new_block');
  });

  testWidgets('removing a block removes its card', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(controller: controller, preset: presetA),
    ));

    await tester.tap(find.byKey(const Key('remove-block-button-0')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('block-card-0')), findsNothing);
  });

  testWidgets('toggling a block\'s enabled switch updates the draft',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(controller: controller, preset: presetA),
    ));

    var switchWidget = tester
        .widget<Switch>(find.byKey(const Key('block-enabled-switch-0')));
    expect(switchWidget.value, true);

    await tester.tap(find.byKey(const Key('block-enabled-switch-0')));
    await tester.pumpAndSettle();

    switchWidget = tester
        .widget<Switch>(find.byKey(const Key('block-enabled-switch-0')));
    expect(switchWidget.value, false);

    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final cmd = fakeClient.sentCommands
        .where((c) => c.type == 'update_preset')
        .single
        .command as UpdatePresetCommand;
    expect(cmd.blocks!.single.enabled, false);
  });

  testWidgets('adding a param adds a new editable row', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(controller: controller, preset: presetA),
    ));

    expect(find.byKey(const Key('param-row-decay')), findsOneWidget);
    expect(find.byKey(const Key('param-row-param')), findsNothing);

    await tester.tap(find.byKey(const Key('add-param-button')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('param-row-param')), findsOneWidget);
  });
}
