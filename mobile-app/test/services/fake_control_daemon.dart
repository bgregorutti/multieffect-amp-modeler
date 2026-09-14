/// A small, genuinely-networked fake control daemon used by
/// `daemon_client_test.dart`.
///
/// This is a real `dart:io` `HttpServer` + `WebSocketTransformer` listening
/// on a loopback socket -- not an in-process fake transport -- so the test
/// exercises `DaemonClient`'s actual WebSocket framing/JSON codec over a
/// real socket. Mirrors the same principle
/// `control-daemon/tests/test_upload_memory.py` uses for its own real-server
/// benchmark (see that file's docstring for why an in-process fake can hide
/// real protocol/framing bugs a real socket would catch).
///
/// It understands just enough of the documented protocol (see
/// `control-daemon/README.md`) to be useful: `hello` -> `state_snapshot`,
/// and enough app-only commands to prove `DaemonClient` sends/receives
/// correctly, plus the ability for the test to trigger an out-of-band
/// broadcast (simulating another client's edit, or a footswitch press).
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';

class FakeControlDaemon {
  HttpServer? _server;
  final _sockets = <WebSocket>[];

  /// In-memory state, shaped like `DaemonState`. Deliberately minimal --
  /// just enough fields for the test scenarios that exercise it.
  Map<String, dynamic> state = {
    'version': 2,
    'rigs': <dynamic>[],
    'assets': <String, dynamic>{},
    'footswitch_mapping': <String, dynamic>{},
    'active_rig_index': 0,
    'active_preset_index': 0,
    'active_rig_id': null,
    'active_preset_id': null,
    'bypass': false,
    'tempo_bpm': null,
  };

  int get port => _server!.port;
  Uri get wsUri => Uri.parse('ws://127.0.0.1:$port/ws');

  Future<void> start() async {
    _server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    unawaited(_serve());
  }

  Future<void> _serve() async {
    await for (final request in _server!) {
      if (WebSocketTransformer.isUpgradeRequest(request)) {
        final socket = await WebSocketTransformer.upgrade(request);
        _handleSocket(socket);
      } else {
        request.response.statusCode = 404;
        await request.response.close();
      }
    }
  }

  void _handleSocket(WebSocket socket) {
    _sockets.add(socket);
    socket.listen((raw) {
      final json = jsonDecode(raw as String) as Map<String, dynamic>;
      _handleMessage(socket, json);
    }, onDone: () {
      _sockets.remove(socket);
    });
  }

  void _send(WebSocket socket, Map<String, dynamic> message) {
    socket.add(jsonEncode(message));
  }

  void _broadcastStateChanged(String reason) {
    final message = {'type': 'state_changed', 'reason': reason, 'state': state};
    for (final socket in _sockets) {
      _send(socket, message);
    }
  }

  void _handleMessage(WebSocket socket, Map<String, dynamic> json) {
    final type = json['type'] as String?;
    switch (type) {
      case 'hello':
        _send(socket, {'type': 'state_snapshot', 'state': state});
      case 'create_rig':
        final rigs = state['rigs'] as List<dynamic>;
        final rigId = 'rig-${rigs.length + 1}';
        final rig = {
          'id': rigId,
          'name': json['name'],
          'chain': json['chain'] ?? <dynamic>[],
          'presets': <dynamic>[],
        };
        rigs.add(rig);
        _send(socket, {
          'type': 'command_ok',
          'command': 'create_rig',
          'result': {'rig': rig},
        });
        _broadcastStateChanged('create_rig');
      case 'create_preset':
        final rigs = state['rigs'] as List<dynamic>;
        final rigId = json['rig_id'] as String?;
        final rig = rigs.cast<Map<String, dynamic>>().firstWhere(
              (r) => r['id'] == rigId,
              orElse: () => <String, dynamic>{},
            );
        if (rig.isEmpty) {
          _send(socket, {
            'type': 'error',
            'code': 'not_found',
            'message': 'no such rig: $rigId',
          });
          return;
        }
        final presets = rig['presets'] as List<dynamic>;
        final preset = {
          'id': 'preset-${presets.length + 1}',
          'name': json['name'],
          'block_states': json['block_states'] ?? <String, dynamic>{},
          'created_at': 0.0,
          'updated_at': 0.0,
        };
        presets.add(preset);
        _send(socket, {
          'type': 'command_ok',
          'command': 'create_preset',
          'result': {'preset': preset},
        });
        _broadcastStateChanged('create_preset');
      case 'set_bypass':
        state['bypass'] = json['bypass'];
        _send(socket, {
          'type': 'command_ok',
          'command': 'set_bypass',
          'result': {'bypass': state['bypass']},
        });
        _broadcastStateChanged('set_bypass');
      case 'delete_preset':
        final rigs = state['rigs'] as List<dynamic>;
        final presetId = json['preset_id'] as String?;
        final rig = rigs.cast<Map<String, dynamic>>().firstWhere(
              (r) => (r['presets'] as List<dynamic>)
                  .cast<Map<String, dynamic>>()
                  .any((p) => p['id'] == presetId),
              orElse: () => <String, dynamic>{},
            );
        if (presetId == null || rig.isEmpty) {
          _send(socket, {
            'type': 'error',
            'code': 'not_found',
            'message': 'no such preset: $presetId',
          });
          return;
        }
        (rig['presets'] as List<dynamic>)
            .removeWhere((p) => (p as Map<String, dynamic>)['id'] == presetId);
        _send(socket, {
          'type': 'command_ok',
          'command': 'delete_preset',
          'result': {},
        });
        _broadcastStateChanged('delete_preset');
      case 'trigger_role_forbidden_error':
        // Test-only hook: not a real daemon message type, used to exercise
        // DaemonClient's error-reply handling deterministically.
        _send(socket, {
          'type': 'error',
          'code': 'role_forbidden',
          'message': "role 'display' may not send 'set_bypass'",
        });
      default:
        _send(socket, {
          'type': 'error',
          'code': 'unknown_message_type',
          'message': 'unrecognized message type: $type',
        });
    }
  }

  /// Simulate a mutation from another client entirely (e.g. a second app
  /// instance, or a footswitch press) -- broadcasts a state_changed without
  /// any command_ok, to any currently-connected socket.
  void simulateExternalChange(Map<String, dynamic> newState, String reason) {
    state = newState;
    _broadcastStateChanged(reason);
  }

  Future<void> stop() async {
    for (final socket in List.of(_sockets)) {
      await socket.close();
    }
    await _server?.close(force: true);
  }
}
