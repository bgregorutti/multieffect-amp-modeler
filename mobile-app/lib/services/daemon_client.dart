import 'dart:async';
import 'dart:convert';

import 'package:web_socket_channel/web_socket_channel.dart';

import '../models/daemon_state.dart';
import '../models/ws_messages.dart';

enum ConnectionStatus { disconnected, connecting, connected, error }

/// Thrown when a command sent to the daemon gets back a typed `error` reply.
class DaemonCommandError implements Exception {
  final String code;
  final String message;

  const DaemonCommandError(this.code, this.message);

  @override
  String toString() => 'DaemonCommandError($code: $message)';
}

/// Interface implemented by [DaemonClient] and by test doubles used in
/// screen widget tests, so screens can be exercised without a real socket.
abstract class DaemonClientBase {
  ConnectionStatus get status;
  DaemonState get state;

  Stream<ConnectionStatus> get statusStream;
  Stream<DaemonState> get stateStream;

  Future<void> connect();
  void dispose();

  Future<Map<String, dynamic>> createPreset(CreatePresetCommand cmd);
  Future<Map<String, dynamic>> updatePreset(UpdatePresetCommand cmd);
  Future<Map<String, dynamic>> deletePreset(DeletePresetCommand cmd);
  Future<Map<String, dynamic>> selectPreset(SelectPresetCommand cmd);
  Future<Map<String, dynamic>> createRig(CreateRigCommand cmd);
  Future<Map<String, dynamic>> updateRig(UpdateRigCommand cmd);
  Future<Map<String, dynamic>> deleteRig(DeleteRigCommand cmd);
  Future<Map<String, dynamic>> reorderRigs(ReorderRigsCommand cmd);
  Future<Map<String, dynamic>> setBypass(SetBypassCommand cmd);
  Future<Map<String, dynamic>> setFootswitchMapping(
      SetFootswitchMappingCommand cmd);
  Future<Map<String, dynamic>> registerAsset(RegisterAssetCommand cmd);
  Future<Map<String, dynamic>> renameAsset(RenameAssetCommand cmd);
  Future<Map<String, dynamic>> setBlockParam(SetBlockParamCommand cmd);
  Future<Map<String, dynamic>> listBlockTypes(ListBlockTypesCommand cmd);
}

/// Wraps a WebSocket connection to the control daemon.
///
/// Connects, sends `hello` with role `"app"`, exposes the live
/// [DaemonState] rebuilt from every `state_snapshot`/`state_changed`
/// broadcast (this class never maintains separate optimistic local state --
/// per the protocol, every mutation, including this app's own commands and
/// footswitch presses from elsewhere, is reflected back as a broadcast and
/// that broadcast is the only thing that updates [state]), and has one
/// method per app-only command that resolves on the matching `command_ok`
/// or throws on `error`.
class DaemonClient extends DaemonClientBase {
  final Uri uri;
  final String clientName;
  final Duration commandTimeout;

  WebSocketChannel? _channel;
  StreamSubscription<dynamic>? _subscription;

  final _statusController = StreamController<ConnectionStatus>.broadcast();
  final _stateController = StreamController<DaemonState>.broadcast();

  ConnectionStatus _status = ConnectionStatus.disconnected;
  DaemonState _state = DaemonState.empty;

  /// Pending command replies, keyed by command `type`. The protocol replies
  /// to a command with a `command_ok` carrying that same `command` string,
  /// and this daemon only ever has one command of a given type in flight at
  /// a time from this simple sequential client, so keying on type (rather
  /// than a request id the protocol doesn't have) is sufficient here.
  final Map<String, Completer<Map<String, dynamic>>> _pendingByType = {};

  /// Commands whose `command_ok` has arrived but whose completer is
  /// deliberately not resolved yet -- see [_onData]'s `CommandOkMessage`
  /// case for why.
  final List<MapEntry<Completer<Map<String, dynamic>>, Map<String, dynamic>>>
      _awaitingBroadcast = [];

  /// Command types the daemon never follows with a `state_changed`
  /// broadcast (a pure query -- see `list_block_types`'s own docstring:
  /// "never mutates state or notifies"). Everything else is a mutation and
  /// is always followed by one on this same connection.
  static const _queryOnlyCommandTypes = {'list_block_types'};

  DaemonClient({
    required this.uri,
    this.clientName = 'mobile-app',
    this.commandTimeout = const Duration(seconds: 10),
  });

  @override
  ConnectionStatus get status => _status;

  @override
  DaemonState get state => _state;

  @override
  Stream<ConnectionStatus> get statusStream => _statusController.stream;

  @override
  Stream<DaemonState> get stateStream => _stateController.stream;

  void _setStatus(ConnectionStatus s) {
    _status = s;
    if (!_statusController.isClosed) _statusController.add(s);
  }

  void _setState(DaemonState s) {
    _state = s;
    if (!_stateController.isClosed) _stateController.add(s);
  }

  @override
  Future<void> connect() async {
    _setStatus(ConnectionStatus.connecting);
    try {
      final channel = WebSocketChannel.connect(uri);
      _channel = channel;
      await channel.ready;

      _subscription = channel.stream.listen(
        _onData,
        onError: (Object err, StackTrace st) {
          _setStatus(ConnectionStatus.error);
          _failAllPending('connection error: $err');
        },
        onDone: () {
          _setStatus(ConnectionStatus.disconnected);
          _failAllPending('connection closed');
        },
      );

      _send(HelloMessage(role: 'app', clientName: clientName).toJson());
      _setStatus(ConnectionStatus.connected);
    } catch (e) {
      _setStatus(ConnectionStatus.error);
      rethrow;
    }
  }

  void _onData(dynamic data) {
    final Map<String, dynamic> json =
        jsonDecode(data as String) as Map<String, dynamic>;
    final message = ServerMessage.fromJson(json);

    switch (message) {
      case StateSnapshotMessage(:final state):
        _setState(DaemonState.fromJson(state));
      case StateChangedMessage(:final state):
        _setState(DaemonState.fromJson(state));
        // A mutating command's completer was deliberately held back until
        // here (see CommandOkMessage below) -- now that [state] reflects
        // it, callers awaiting that command can safely act on [state]
        // (e.g. navigate to a screen that reads it) without racing this
        // broadcast. FIFO, oldest first: on this client's one-in-flight-
        // per-type usage pattern there is normally at most one entry, but
        // this stays correct even if that ever briefly isn't true.
        if (_awaitingBroadcast.isNotEmpty) {
          final entry = _awaitingBroadcast.removeAt(0);
          entry.key.complete(entry.value);
        }
      case CommandOkMessage(:final command, :final result):
        final completer = _pendingByType.remove(command);
        if (completer == null) break;
        if (_queryOnlyCommandTypes.contains(command)) {
          // No broadcast will ever follow a pure query -- resolve now.
          completer.complete(result);
        } else {
          // Resolving immediately here would let an `await` on this
          // command's Future (and whatever it does next, e.g. navigating
          // to a screen that reads `state`) run before the state_changed
          // broadcast this same mutation triggers has been processed --
          // command_ok and that broadcast arrive as two separate WebSocket
          // messages, and completing a Future only *schedules* its
          // continuation as a microtask, which (at least on the web
          // target) reliably runs before the next already-arrived message
          // is dispatched. Concretely: saving a rig edit and immediately
          // navigating back could show the *previous* value, because the
          // pop happened before `state` had actually been updated with the
          // edit. Held here and resolved from the broadcast case above
          // instead, once `state` genuinely reflects this command.
          _awaitingBroadcast.add(MapEntry(completer, result));
        }
      case ErrorMessage(:final code, :final message, :final inReplyTo):
        // The daemon doesn't currently echo which command an error is a
        // reply to for every error path (e.g. validation errors can arise
        // before a `command` field is known), so first try `inReplyTo` and
        // otherwise fail the oldest pending command -- good enough for this
        // client's one-in-flight-per-type usage pattern.
        final completer = inReplyTo != null
            ? _pendingByType.remove(inReplyTo)
            : (_pendingByType.isNotEmpty
                ? _pendingByType.remove(_pendingByType.keys.first)
                : null);
        completer?.completeError(DaemonCommandError(code, message));
      case UnknownMessage():
        break;
    }
  }

  void _failAllPending(String reason) {
    for (final completer in _pendingByType.values) {
      if (!completer.isCompleted) {
        completer.completeError(DaemonCommandError('connection_lost', reason));
      }
    }
    _pendingByType.clear();
    // A command already acknowledged with command_ok but still waiting on
    // its state_changed (see CommandOkMessage in _onData) would otherwise
    // hang forever if the connection drops before that broadcast arrives.
    for (final entry in _awaitingBroadcast) {
      if (!entry.key.isCompleted) {
        entry.key.completeError(DaemonCommandError('connection_lost', reason));
      }
    }
    _awaitingBroadcast.clear();
  }

  void _send(Map<String, dynamic> json) {
    _channel?.sink.add(jsonEncode(json));
  }

  Future<Map<String, dynamic>> _sendCommand(DaemonCommand cmd) {
    final completer = Completer<Map<String, dynamic>>();
    _pendingByType[cmd.type] = completer;
    _send(cmd.toJson());
    return completer.future.timeout(
      commandTimeout,
      onTimeout: () {
        _pendingByType.remove(cmd.type);
        // Covers the rare case where command_ok arrived (moving this
        // completer into _awaitingBroadcast) but the daemon's own
        // state_changed for it never did -- without this, the entry would
        // sit there forever even though the caller has already timed out.
        _awaitingBroadcast.removeWhere((entry) => entry.key == completer);
        throw DaemonCommandError(
          'timeout',
          'no reply to "${cmd.type}" within $commandTimeout',
        );
      },
    );
  }

  @override
  Future<Map<String, dynamic>> createPreset(CreatePresetCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> updatePreset(UpdatePresetCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> deletePreset(DeletePresetCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> selectPreset(SelectPresetCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> createRig(CreateRigCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> updateRig(UpdateRigCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> deleteRig(DeleteRigCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> reorderRigs(ReorderRigsCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> setBypass(SetBypassCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> setFootswitchMapping(
          SetFootswitchMappingCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> registerAsset(RegisterAssetCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> renameAsset(RenameAssetCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> setBlockParam(SetBlockParamCommand cmd) =>
      _sendCommand(cmd);

  @override
  Future<Map<String, dynamic>> listBlockTypes(ListBlockTypesCommand cmd) =>
      _sendCommand(cmd);

  @override
  void dispose() {
    _subscription?.cancel();
    _channel?.sink.close();
    _failAllPending('disposed');
    _statusController.close();
    _stateController.close();
  }
}
