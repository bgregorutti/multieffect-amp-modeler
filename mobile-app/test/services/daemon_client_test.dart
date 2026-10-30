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

  test('reconnects on its own after the socket drops', () async {
    // The whole point: a Wi-Fi blip must not leave the app dead until
    // someone notices and taps Connect.
    client = DaemonClient(
      uri: daemon.wsUri,
      clientName: 'test-app',
      reconnectInitialDelay: const Duration(milliseconds: 50),
      reconnectMaxDelay: const Duration(milliseconds: 100),
    );
    await client.connect();
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(client.status, ConnectionStatus.connected);

    await daemon.dropConnections();
    await Future<void>.delayed(const Duration(milliseconds: 500));

    expect(client.status, ConnectionStatus.connected,
        reason: 'should have reconnected without any manual intervention');
  });

  test('rebuilds state from the snapshot after reconnecting', () async {
    client = DaemonClient(
      uri: daemon.wsUri,
      clientName: 'test-app',
      reconnectInitialDelay: const Duration(milliseconds: 50),
      reconnectMaxDelay: const Duration(milliseconds: 100),
    );
    await client.connect();
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(client.state.bypass, false);

    // State changed while the app was away -- the reconnect's own
    // state_snapshot is what has to bring it back in sync.
    daemon.state = {...daemon.state, 'bypass': true};
    await daemon.dropConnections();
    await Future<void>.delayed(const Duration(milliseconds: 500));

    expect(client.status, ConnectionStatus.connected);
    expect(client.state.bypass, true,
        reason: 'reconnect must resync state, not keep the stale copy');
  });

  test('keeps retrying while the daemon is down, then connects when it '
      'comes back', () async {
    // Bind a fresh server on a known port so it can be stopped and restarted
    // at the same address, which is what a daemon/Pi restart looks like.
    final restartable = FakeControlDaemon();
    await restartable.start();
    final uri = restartable.wsUri;

    client = DaemonClient(
      uri: uri,
      clientName: 'test-app',
      reconnectInitialDelay: const Duration(milliseconds: 50),
      reconnectMaxDelay: const Duration(milliseconds: 100),
    );
    await client.connect();
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(client.status, ConnectionStatus.connected);

    await restartable.stop();
    await Future<void>.delayed(const Duration(milliseconds: 300));
    expect(client.status, isNot(ConnectionStatus.connected));

    // Same port again: the retry loop should find it.
    final revived = FakeControlDaemon();
    await revived.startOn(uri.port);
    await Future<void>.delayed(const Duration(milliseconds: 600));

    expect(client.status, ConnectionStatus.connected,
        reason: 'retry loop should pick the daemon back up unaided');
    await revived.stop();
  });

  test('dispose stops the retry loop', () async {
    client = DaemonClient(
      uri: daemon.wsUri,
      clientName: 'test-app',
      reconnectInitialDelay: const Duration(milliseconds: 50),
      reconnectMaxDelay: const Duration(milliseconds: 100),
    );
    await client.connect();
    await Future<void>.delayed(const Duration(milliseconds: 200));
    final connectionsWhileAlive = daemon.acceptedConnections;

    client.dispose();
    await daemon.dropConnections();
    // Long enough that several retries would have fired had dispose not
    // stopped them -- a disposed client reconnecting would leak a socket
    // (and, via main.dart's Settings flow, fight the client that replaced it).
    await Future<void>.delayed(const Duration(milliseconds: 400));

    expect(daemon.acceptedConnections, connectionsWhileAlive,
        reason: 'a disposed client must not keep reconnecting');
  });

  test(
    'two commands of the same type sent back to back both get their reply',
    () async {
      // Rapid preset taps: the second command goes out while the first is
      // still waiting. Replies carry no id, so they are matched by type in
      // order -- the first must not be orphaned by the second.
      client = DaemonClient(
        uri: daemon.wsUri,
        clientName: 'test-app',
        commandTimeout: const Duration(seconds: 2),
      );
      await client.connect();
      await Future<void>.delayed(const Duration(milliseconds: 200));

      final first = client.setBypass(const SetBypassCommand(bypass: true));
      final second = client.setBypass(const SetBypassCommand(bypass: false));

      expect((await first)['bypass'], true);
      expect((await second)['bypass'], false);
    },
  );

  test('a command while disconnected fails at once instead of timing out', () async {
    client = DaemonClient(
      uri: daemon.wsUri,
      clientName: 'test-app',
      commandTimeout: const Duration(seconds: 5),
    );
    // Never connected.
    final stopwatch = Stopwatch()..start();
    await expectLater(
      client.setBypass(const SetBypassCommand(bypass: true)),
      throwsA(isA<DaemonCommandError>()
          .having((e) => e.code, 'code', 'not_connected')),
    );
    expect(stopwatch.elapsed, lessThan(const Duration(seconds: 1)));
  });

  test(
    'overlapping connects (e.g. Android firing resumed twice) leave one '
    'working connection',
    () async {
      client = DaemonClient(
        uri: daemon.wsUri,
        clientName: 'test-app',
        commandTimeout: const Duration(seconds: 2),
      );
      final statuses = <ConnectionStatus>[];
      client.statusStream.listen(statuses.add);
      // Not awaited one by one: the second starts while the first is still
      // opening its socket.
      await Future.wait([
        client.connect(),
        client.connect(),
        client.connect(),
      ]);
      await Future<void>.delayed(const Duration(milliseconds: 300));

      for (final bypass in [true, false, true]) {
        final result = await client.setBypass(SetBypassCommand(bypass: bypass));
        expect(result['bypass'], bypass);
      }
      expect(daemon.helloCount, daemon.openConnections,
          reason: 'every open socket must be one that said hello');
      expect(daemon.openConnections, 1);
      // One socket for all three calls, and no spurious drop: each extra
      // socket used to be closed by the next call, and its listener then
      // reported "connection closed" -- failing whatever command was in
      // flight -- and scheduled yet another reconnect.
      expect(daemon.acceptedConnections, 1);
      expect(statuses, isNot(contains(ConnectionStatus.disconnected)));
      expect(statuses, isNot(contains(ConnectionStatus.error)));
    },
  );

  test('a command that gets no reply makes the client reconnect', () async {
    client = DaemonClient(
      uri: daemon.wsUri,
      clientName: 'test-app',
      commandTimeout: const Duration(milliseconds: 500),
    );
    await client.connect();
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(daemon.acceptedConnections, 1);

    daemon.ignoredTypes.add('set_bypass');
    await expectLater(
      client.setBypass(const SetBypassCommand(bypass: true)),
      throwsA(isA<DaemonCommandError>().having((e) => e.code, 'code', 'timeout')),
    );
    await Future<void>.delayed(const Duration(milliseconds: 300));
    expect(daemon.acceptedConnections, 2);
    expect(client.status, ConnectionStatus.connected);

    daemon.ignoredTypes.clear();
    final result = await client.setBypass(const SetBypassCommand(bypass: true));
    expect(result['bypass'], true);
  });
}
