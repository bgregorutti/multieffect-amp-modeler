import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/footswitch_action.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/footswitch_mapping_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  Future<FakeDaemonClient> pumpScreen(WidgetTester tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);
    await tester.pumpWidget(MaterialApp(
      home: FootswitchMappingScreen(controller: controller),
    ));
    return fakeClient;
  }

  SetFootswitchMappingCommand lastMapping(FakeDaemonClient client) =>
      client.sentCommands
          .where((c) => c.type == 'set_footswitch_mapping')
          .single
          .command as SetFootswitchMappingCommand;

  testWidgets('renders one row per mapped switch', (tester) async {
    await pumpScreen(tester);

    // The fixture is the pedal's four-switch layout: left/right step
    // presets, up/down step rigs.
    for (var i = 0; i < 4; i++) {
      expect(find.byKey(Key('switch-row-$i')), findsOneWidget);
    }
    expect(find.byKey(const Key('switch-row-4')), findsNothing);
  });

  testWidgets('no index field is shown for a stepping action', (tester) async {
    await pumpScreen(tester);
    // Only select_preset needs an index; next/prev stepping does not.
    expect(find.byKey(const Key('select-preset-index-field-0')), findsNothing);
  });

  testWidgets('adding a switch appends a new row', (tester) async {
    await pumpScreen(tester);

    expect(find.byKey(const Key('switch-row-4')), findsNothing);
    await tester.tap(find.byKey(const Key('add-switch-button')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('switch-row-4')), findsOneWidget);
  });

  testWidgets('removing a switch removes its row', (tester) async {
    await pumpScreen(tester);

    await tester.tap(find.byKey(const Key('remove-switch-button-1')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('switch-row-1')), findsNothing);
  });

  testWidgets(
    'saving sends set_footswitch_mapping with the exact current draft',
    (tester) async {
      final fakeClient = await pumpScreen(tester);

      await tester.tap(find.byKey(const Key('save-mapping-button')));
      await tester.pumpAndSettle();

      final cmd = lastMapping(fakeClient);
      expect(cmd.mapping[0], const NextPresetAction());
      expect(cmd.mapping[1], const PrevPresetAction());
      expect(cmd.mapping[2], const NextRigAction());
      expect(cmd.mapping[3], const PrevRigAction());
    },
  );

  testWidgets(
    'switching an action to select_preset reveals its index field and the '
    'edited index is what gets sent',
    (tester) async {
      final fakeClient = await pumpScreen(tester);

      await tester.tap(find.byKey(const Key('action-kind-dropdown-0')));
      await tester.pumpAndSettle();
      await tester.tap(find.text('select_preset').last);
      await tester.pumpAndSettle();

      await tester.enterText(
        find.byKey(const Key('select-preset-index-field-0')),
        '3',
      );
      await tester.tap(find.byKey(const Key('save-mapping-button')));
      await tester.pumpAndSettle();

      expect(lastMapping(fakeClient).mapping[0],
          const SelectPresetAction(index: 3));
    },
  );
}
