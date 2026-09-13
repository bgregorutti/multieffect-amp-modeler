import 'dart:async';

import 'package:flutter/foundation.dart';

import '../models/daemon_state.dart';
import '../services/daemon_client.dart';

/// A [ChangeNotifier] wrapping a [DaemonClientBase], so widgets can react to
/// connection-status and state changes via [ListenableBuilder]/
/// [AnimatedBuilder] without pulling in a separate state-management package.
///
/// This deliberately holds no state of its own beyond what it mirrors from
/// the client -- [state] and [status] always reflect the daemon's latest
/// broadcast, per the "no separate optimistic local state" principle.
class DaemonStateController extends ChangeNotifier {
  final DaemonClientBase client;

  StreamSubscription<DaemonState>? _stateSub;
  StreamSubscription<ConnectionStatus>? _statusSub;

  DaemonStateController(this.client) {
    _stateSub = client.stateStream.listen((_) => notifyListeners());
    _statusSub = client.statusStream.listen((_) => notifyListeners());
  }

  DaemonState get state => client.state;
  ConnectionStatus get status => client.status;

  Future<void> connect() => client.connect();

  @override
  void dispose() {
    _stateSub?.cancel();
    _statusSub?.cancel();
    super.dispose();
  }
}
