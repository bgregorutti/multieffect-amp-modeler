import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/footswitch_action.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/footswitch_mapping_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  testWidgets('renders one row per mapped switch', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: FootswitchMappingScreen(controller: controller),
    ));

    expect(find.byKey(const Key('switch-row-0')), findsOneWidget);
    expect(find.byKey(const Key('switch-row-1')), findsOneWidget);
    // select_slot on switch 0 shows a slot number field.
    expect(find.byKey(const Key('select-slot-field-0')), findsOneWidget);
  });

  testWidgets('adding a switch appends a new row defaulted to toggle_bypass',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: FootswitchMappingScreen(controller: controller),
    ));

    expect(find.byKey(const Key('switch-row-2')), findsNothing);
    await tester.tap(find.byKey(const Key('add-switch-button')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('switch-row-2')), findsOneWidget);
  });

  testWidgets('removing a switch removes its row', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: FootswitchMappingScreen(controller: controller),
    ));

    await tester.tap(find.byKey(const Key('remove-switch-button-1')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('switch-row-1')), findsNothing);
  });

  testWidgets(
    'saving sends set_footswitch_mapping with the exact current draft',
    (tester) async {
      final fakeClient = FakeDaemonClient(state: sampleState);
      final controller = DaemonStateController(fakeClient);

      await tester.pumpWidget(MaterialApp(
        home: FootswitchMappingScreen(controller: controller),
      ));

      await tester.tap(find.byKey(const Key('save-mapping-button')));
      await tester.pumpAndSettle();

      final commands = fakeClient.sentCommands
          .where((c) => c.type == 'set_footswitch_mapping')
          .toList();
      expect(commands, hasLength(1));
      final cmd =
          commands.single.command as SetFootswitchMappingCommand;
      expect(cmd.mapping[0], const SelectSlotAction(slot: 0));
      expect(cmd.mapping[1], const NextBankAction());
    },
  );

  testWidgets(
    'changing a slot number for a select_slot switch updates the draft sent',
    (tester) async {
      final fakeClient = FakeDaemonClient(state: sampleState);
      final controller = DaemonStateController(fakeClient);

      await tester.pumpWidget(MaterialApp(
        home: FootswitchMappingScreen(controller: controller),
      ));

      await tester.enterText(
        find.byKey(const Key('select-slot-field-0')),
        '3',
      );
      await tester.tap(find.byKey(const Key('save-mapping-button')));
      await tester.pumpAndSettle();

      final cmd = fakeClient.sentCommands
          .where((c) => c.type == 'set_footswitch_mapping')
          .single
          .command as SetFootswitchMappingCommand;
      expect(cmd.mapping[0], const SelectSlotAction(slot: 3));
    },
  );
}
