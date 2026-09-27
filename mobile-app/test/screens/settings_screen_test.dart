import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/screens/settings_screen.dart';
import 'package:mobile_app/services/daemon_endpoint_store.dart';

void main() {
  const initial = DaemonEndpoint(host: '127.0.0.1', port: 8765);

  testWidgets('pre-fills the current endpoint', (tester) async {
    await tester.pumpWidget(MaterialApp(
      home: SettingsScreen(currentEndpoint: initial, onSave: (_) {}),
    ));

    expect(find.text('127.0.0.1'), findsOneWidget);
    expect(find.text('8765'), findsOneWidget);
  });

  testWidgets('rejects an empty host and does not call onSave', (tester) async {
    DaemonEndpoint? saved;
    await tester.pumpWidget(MaterialApp(
      home: SettingsScreen(
        currentEndpoint: initial,
        onSave: (endpoint) => saved = endpoint,
      ),
    ));

    await tester.enterText(find.byKey(const Key('host-field')), '');
    await tester.tap(find.byKey(const Key('save-settings-button')));
    await tester.pump();

    expect(saved, isNull);
    expect(find.text('Required'), findsOneWidget);
  });

  testWidgets('rejects an out-of-range port and does not call onSave',
      (tester) async {
    DaemonEndpoint? saved;
    await tester.pumpWidget(MaterialApp(
      home: SettingsScreen(
        currentEndpoint: initial,
        onSave: (endpoint) => saved = endpoint,
      ),
    ));

    await tester.enterText(find.byKey(const Key('port-field')), '70000');
    await tester.tap(find.byKey(const Key('save-settings-button')));
    await tester.pump();

    expect(saved, isNull);
    expect(find.text('Enter a port between 1 and 65535'), findsOneWidget);
  });

  testWidgets('saves the trimmed host and parsed port', (tester) async {
    DaemonEndpoint? saved;
    await tester.pumpWidget(MaterialApp(
      home: SettingsScreen(
        currentEndpoint: initial,
        onSave: (endpoint) => saved = endpoint,
      ),
    ));

    await tester.enterText(
        find.byKey(const Key('host-field')), '  192.168.1.42  ');
    await tester.enterText(find.byKey(const Key('port-field')), '9000');
    await tester.tap(find.byKey(const Key('save-settings-button')));
    await tester.pump();

    expect(saved, const DaemonEndpoint(host: '192.168.1.42', port: 9000));
  });
}
