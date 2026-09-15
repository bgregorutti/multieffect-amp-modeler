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
      expect(client.state.rigs, isEmpty);
      expect(client.state.bypass, false);
    },
  );

  test(
    'a command gets its command_ok reply and the broadcast updates state',
    () async {
      client = DaemonClient(uri: daemon.wsUri, clientName: 'test-app');
      await client.connect();
      await Future<void>.delayed(const Duration(milliseconds: 200));

      final stateEvents = <int>[]; // number of rigs seen at each update
      client.stateStream.listen((s) => stateEvents.add(s.rigs.length));

      await client.createRig(const CreateRigCommand(name: 'Ampeg SVT'));
      final result = await client.createPreset(
        const CreatePresetCommand(rigId: 'rig-1', name: 'Drive'),
      );

      expect(result['preset']['name'], 'Drive');

      // The client's exposed state is rebuilt from the state_changed
      // broadcast, which -- per protocol -- arrives on this same connection
      // just *after* the direct command_ok reply already awaited above, so
      // give the event loop a beat to deliver and process it.
      await Future<void>.delayed(const Duration(milliseconds: 100));

      expect(client.state.rigs, hasLength(1));
      expect(client.state.rigs.single.presets, hasLength(1));
      expect(client.state.rigs.single.presets.single.name, 'Drive');
      expect(stateEvents, isNotEmpty);
    },
  );

  test(
    'a mutating command\'s Future does not resolve until state already '
    'reflects it -- no grace-period delay needed',
    () async {
      // Regression test: command_ok and the state_changed it triggers are
      // two separate WebSocket messages; completing a command's Future as
      // soon as command_ok arrives let its continuation (e.g. a screen
      // saving an edit and immediately navigating to read `state`) run
      // before the broadcast had been processed, so a caller could
      // observe stale state right after a successful, awaited command --
      // concretely, saving a rig edit and immediately reopening it could
      // show the value from before the edit. DaemonClient now holds a
      // mutating command's completer until its state_changed has been
      // applied (see _onData's CommandOkMessage case) instead of resolving
      // on command_ok alone.
      client = DaemonClient(uri: daemon.wsUri, clientName: 'test-app');
      await client.connect();
      await Future<void>.delayed(const Duration(milliseconds: 200));

      expect(client.state.bypass, false);

      await client.setBypass(const SetBypassCommand(bypass: true));

      // No delay here on purpose -- if this were flaky/failing, the fix
      // regressed back to resolving on command_ok alone.
      expect(client.state.bypass, true);
    },
  );

  test('an error reply surfaces as a DaemonCommandError', () async {
    client = DaemonClient(uri: daemon.wsUri, clientName: 'test-app');
    await client.connect();
    await Future<void>.delayed(const Duration(milliseconds: 200));

    await expectLater(
      client.deletePreset(
        const DeletePresetCommand(rigId: 'rig-1', presetId: 'no-such-id'),
      ),
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
