import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/screens/connection_screen.dart';
import 'package:mobile_app/services/daemon_client.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  testWidgets('shows connected status, active preset, bypass and tempo',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: ConnectionScreen(controller: controller),
    ));

    expect(find.text('Connected'), findsOneWidget);
    expect(find.text('Ambient Swell'), findsOneWidget);
    expect(find.text('120.0 BPM'), findsOneWidget);

    final bypassSwitch =
        tester.widget<SwitchListTile>(find.byKey(const Key('bypass-switch')));
    expect(bypassSwitch.value, false);
  });

  testWidgets('shows a connect button while disconnected, which reconnects',
      (tester) async {
    final fakeClient =
        FakeDaemonClient(status: ConnectionStatus.disconnected);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: ConnectionScreen(controller: controller),
    ));

    expect(find.text('Disconnected'), findsOneWidget);
    expect(find.byKey(const Key('connect-button')), findsOneWidget);

    await tester.tap(find.byKey(const Key('connect-button')));
    await tester.pump();

    expect(find.text('Connected'), findsOneWidget);
  });

  testWidgets('shows "No preset selected" when nothing is active',
      (tester) async {
    final fakeClient = FakeDaemonClient();
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: ConnectionScreen(controller: controller),
    ));

    expect(find.text('No preset selected'), findsOneWidget);
    expect(find.text('No tempo set'), findsOneWidget);
  });
}
