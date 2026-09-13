/// Real integration test for [DaemonClient]: spins up a genuine local
/// `dart:io` WebSocket server (see `fake_control_daemon.dart`) and connects
/// a real `DaemonClient` to `ws://127.0.0.1:<port>/ws`, exercising the
/// actual socket + JSON codec rather than an in-process fake transport.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/services/daemon_client.dart';

import 'fake_control_daemon.dart';

void main() {
  late FakeControlDaemon daemon;
  late DaemonClient client;

  setUp(() async {
    daemon = FakeControlDaemon();
    await daemon.start();
  });

  tearDown(() async {
    client.dispose();
    await daemon.stop();
  });

  test(
    'connects, sends hello, receives a snapshot, and exposes it as state',
    () async {
      client = DaemonClient(uri: daemon.wsUri, clientName: 'test-app');

      expect(client.status, ConnectionStatus.disconnected);

      final statusEvents = <ConnectionStatus>[];
      client.statusStream.listen(statusEvents.add);

      await client.connect();
      // Let the event loop deliver the snapshot reply over the real socket.
      await Future<void>.delayed(const Duration(milliseconds: 200));

      expect(client.status, ConnectionStatus.connected);
      expect(statusEvents, contains(ConnectionStatus.connecting));
      expect(statusEvents, contains(ConnectionStatus.connected));
      expect(client.state.presets, isEmpty);
      expect(client.state.bypass, false);
    },
  );

  test(
    'a command gets its command_ok reply and the broadcast updates state',
    () async {
      client = DaemonClient(uri: daemon.wsUri, clientName: 'test-app');
      await client.connect();
      await Future<void>.delayed(const Duration(milliseconds: 200));

      final stateEvents = <int>[]; // number of presets seen at each update
      client.stateStream.listen((s) => stateEvents.add(s.presets.length));

      final result = await client
          .createPreset(const CreatePresetCommand(name: 'Ambient Swell'));

      expect(result['preset']['name'], 'Ambient Swell');

      // The client's exposed state is rebuilt from the state_changed
      // broadcast, which -- per protocol -- arrives on this same connection
      // just *after* the direct command_ok reply already awaited above, so
      // give the event loop a beat to deliver and process it.
      await Future<void>.delayed(const Duration(milliseconds: 100));

      expect(client.state.presets, hasLength(1));
      expect(client.state.presets.values.first.name, 'Ambient Swell');
      expect(stateEvents, isNotEmpty);
    },
  );

  test('an error reply surfaces as a DaemonCommandError', () async {
    client = DaemonClient(uri: daemon.wsUri, clientName: 'test-app');
    await client.connect();
    await Future<void>.delayed(const Duration(milliseconds: 200));

    await expectLater(
      client.deletePreset(const DeletePresetCommand(presetId: 'no-such-id')),
      throwsA(
        isA<DaemonCommandError>().having((e) => e.code, 'code', 'not_found'),
      ),
    );
  });

  test(
    'a broadcast triggered by another client (not this one\'s own command) '
    'still updates this client\'s exposed state -- e.g. a footswitch press '
    'from elsewhere',
    () async {
      client = DaemonClient(uri: daemon.wsUri, clientName: 'test-app');
      await client.connect();
      await Future<void>.delayed(const Duration(milliseconds: 200));

      expect(client.state.bypass, false);

      daemon.simulateExternalChange({
        ...daemon.state,
        'bypass': true,
      }, 'footswitch_toggle_bypass');

      await Future<void>.delayed(const Duration(milliseconds: 200));

      expect(client.state.bypass, true);
    },
  );

  test('status becomes disconnected when the server closes the socket',
      () async {
    client = DaemonClient(uri: daemon.wsUri, clientName: 'test-app');
    await client.connect();
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(client.status, ConnectionStatus.connected);

    await daemon.stop();
    await Future<void>.delayed(const Duration(milliseconds: 300));

    expect(client.status, ConnectionStatus.disconnected);
  });
}
