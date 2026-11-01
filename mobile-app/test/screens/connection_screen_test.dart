import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/connection_screen.dart';
import 'package:mobile_app/services/daemon_client.dart';
import 'package:mobile_app/services/daemon_endpoint_store.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

const _testEndpoint = DaemonEndpoint(host: '127.0.0.1', port: 8765);

void main() {
  testWidgets('shows connected status, active rig and preset, bypass and tempo',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: ConnectionScreen(
        controller: controller,
        currentEndpoint: _testEndpoint,
        onSaveEndpoint: (_) {},
      ),
    ));

    expect(find.text('Connected'), findsOneWidget);
    // Both levels are shown: which backline is loaded, and which preset of
    // it is live.
    expect(find.text('Ampeg SVT'), findsOneWidget);
    expect(find.text('Clean'), findsOneWidget);
    expect(find.text('Preset 1 of 2'), findsOneWidget);
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
      home: ConnectionScreen(
        controller: controller,
        currentEndpoint: _testEndpoint,
        onSaveEndpoint: (_) {},
      ),
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
      home: ConnectionScreen(
        controller: controller,
        currentEndpoint: _testEndpoint,
        onSaveEndpoint: (_) {},
      ),
    ));

    expect(find.text('No preset selected'), findsOneWidget);
    expect(find.text('No tempo set'), findsOneWidget);
  });

  testWidgets('opening settings and saving a new endpoint calls onSaveEndpoint',
      (tester) async {
    final fakeClient = FakeDaemonClient();
    final controller = DaemonStateController(fakeClient);
    DaemonEndpoint? saved;

    await tester.pumpWidget(MaterialApp(
      home: ConnectionScreen(
        controller: controller,
        currentEndpoint: _testEndpoint,
        onSaveEndpoint: (endpoint) => saved = endpoint,
      ),
    ));

    await tester.tap(find.byKey(const Key('open-settings-button')));
    await tester.pumpAndSettle();

    expect(find.text('Daemon Connection'), findsOneWidget);

    await tester.enterText(
        find.byKey(const Key('host-field')), '192.168.1.42');
    await tester.enterText(find.byKey(const Key('port-field')), '9000');
    await tester.tap(find.byKey(const Key('save-settings-button')));
    await tester.pumpAndSettle();

    expect(saved, const DaemonEndpoint(host: '192.168.1.42', port: 9000));
    // Popped back to the status screen.
    expect(find.text('Pedal Status'), findsOneWidget);
  });

  Future<FakeDaemonClient> pumpScreen(WidgetTester tester,
      {ConnectionStatus status = ConnectionStatus.connected}) async {
    final fakeClient = FakeDaemonClient(state: sampleState, status: status);
    await tester.pumpWidget(MaterialApp(
      home: ConnectionScreen(
        controller: DaemonStateController(fakeClient),
        currentEndpoint: _testEndpoint,
        onSaveEndpoint: (_) {},
      ),
    ));
    await tester.scrollUntilVisible(find.byKey(const Key('poweroff-button')), 100);
    return fakeClient;
  }

  testWidgets('shut down asks for confirmation, then sends poweroff',
      (tester) async {
    final fakeClient = await pumpScreen(tester);
    await tester.tap(find.byKey(const Key('poweroff-button')));
    await tester.pumpAndSettle();
    expect(find.text('Shut down the pedal?'), findsOneWidget);
    expect(fakeClient.sentCommands, isEmpty, reason: 'nothing before confirming');

    await tester.tap(find.byKey(const Key('confirm-power-button')));
    await tester.pumpAndSettle();
    expect(fakeClient.sentCommands.single.type, 'system_power');
    expect((fakeClient.sentCommands.single.command as SystemPowerCommand).action,
        'poweroff');
  });

  testWidgets('cancelling a restart sends nothing', (tester) async {
    final fakeClient = await pumpScreen(tester);
    await tester.tap(find.byKey(const Key('reboot-button')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();
    expect(fakeClient.sentCommands, isEmpty);
  });

  testWidgets('power buttons are disabled while disconnected', (tester) async {
    final fakeClient =
        await pumpScreen(tester, status: ConnectionStatus.disconnected);
    await tester.tap(find.byKey(const Key('reboot-button')));
    await tester.pumpAndSettle();
    expect(find.text('Restart the pedal?'), findsNothing);
    expect(fakeClient.sentCommands, isEmpty);
  });
}
