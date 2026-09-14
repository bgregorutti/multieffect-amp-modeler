/// A fake [DaemonClientBase] test double for screen widget tests -- no real
/// network involved. Records every command sent so tests can assert a UI
/// interaction called the expected client method with the expected
/// arguments.
library;

// The constructor below intentionally assigns differently-named public
// parameters (`state`/`status`) to private backing fields (`_state`/
// `_status`) via an initializer list -- an initializing formal isn't usable
// here since a named parameter can't share a private field's leading
// underscore.
// ignore_for_file: prefer_initializing_formals

import 'dart:async';

import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/services/daemon_client.dart';

class RecordedCommand {
  final String type;
  final Object command;
  const RecordedCommand(this.type, this.command);
}

class FakeDaemonClient extends DaemonClientBase {
  final _stateController = StreamController<DaemonState>.broadcast();
  final _statusController = StreamController<ConnectionStatus>.broadcast();

  final List<RecordedCommand> sentCommands = [];

  /// When set, the next call to any command method throws this instead of
  /// recording/returning normally -- for testing error handling in screens.
  Object? nextError;

  /// Result to return from the next command call (defaults to `{}`).
  Map<String, dynamic> nextResult = const {};

  FakeDaemonClient({
    DaemonState state = DaemonState.empty,
    ConnectionStatus status = ConnectionStatus.connected,
  })  : _state = state,
        _status = status;

  DaemonState _state;
  ConnectionStatus _status;

  @override
  DaemonState get state => _state;

  @override
  ConnectionStatus get status => _status;

  @override
  Stream<DaemonState> get stateStream => _stateController.stream;

  @override
  Stream<ConnectionStatus> get statusStream => _statusController.stream;

  /// Test helper: pushes a new state as though a broadcast arrived.
  void pushState(DaemonState next) {
    _state = next;
    _stateController.add(next);
  }

  void pushStatus(ConnectionStatus next) {
    _status = next;
    _statusController.add(next);
  }

  @override
  Future<void> connect() async {
    pushStatus(ConnectionStatus.connected);
  }

  @override
  void dispose() {
    _stateController.close();
    _statusController.close();
  }

  Future<Map<String, dynamic>> _record(String type, Object command) async {
    sentCommands.add(RecordedCommand(type, command));
    if (nextError != null) {
      final err = nextError!;
      nextError = null;
      throw err;
    }
    return nextResult;
  }

  @override
  Future<Map<String, dynamic>> createPreset(CreatePresetCommand cmd) =>
      _record('create_preset', cmd);

  @override
  Future<Map<String, dynamic>> updatePreset(UpdatePresetCommand cmd) =>
      _record('update_preset', cmd);

  @override
  Future<Map<String, dynamic>> deletePreset(DeletePresetCommand cmd) =>
      _record('delete_preset', cmd);

  @override
  Future<Map<String, dynamic>> selectPreset(SelectPresetCommand cmd) =>
      _record('select_preset', cmd);

  @override
  Future<Map<String, dynamic>> createRig(CreateRigCommand cmd) =>
      _record('create_rig', cmd);

  @override
  Future<Map<String, dynamic>> updateRig(UpdateRigCommand cmd) =>
      _record('update_rig', cmd);

  @override
  Future<Map<String, dynamic>> deleteRig(DeleteRigCommand cmd) =>
      _record('delete_rig', cmd);

  @override
  Future<Map<String, dynamic>> reorderRigs(ReorderRigsCommand cmd) =>
      _record('reorder_rigs', cmd);

  @override
  Future<Map<String, dynamic>> setBypass(SetBypassCommand cmd) =>
      _record('set_bypass', cmd);

  @override
  Future<Map<String, dynamic>> setFootswitchMapping(
          SetFootswitchMappingCommand cmd) =>
      _record('set_footswitch_mapping', cmd);

  @override
  Future<Map<String, dynamic>> registerAsset(RegisterAssetCommand cmd) =>
      _record('register_asset', cmd);
}
