import 'package:shared_preferences/shared_preferences.dart';

/// Where the control daemon lives: host + port, used to build both the
/// WebSocket URI ([wsUri]) and the HTTP base URL ([httpBaseUrl]) for asset
/// uploads. See [DaemonEndpointStore] for how this is persisted so pointing
/// the app at a different daemon doesn't require a rebuild.
class DaemonEndpoint {
  final String host;
  final int port;

  const DaemonEndpoint({required this.host, required this.port});

  Uri get wsUri => Uri.parse('ws://$host:$port/ws');
  Uri get httpBaseUrl => Uri.parse('http://$host:$port');

  @override
  bool operator ==(Object other) =>
      other is DaemonEndpoint && other.host == host && other.port == port;

  @override
  int get hashCode => Object.hash(host, port);

  @override
  String toString() => '$host:$port';
}

/// Persists the user-configured [DaemonEndpoint] across app restarts via
/// `shared_preferences`.
///
/// Previously the daemon's address was a compile-time constant
/// (`--dart-define=DAEMON_HOST=...`), so pointing the app at the pedal's
/// actual address -- its Wi-Fi AP, a shared LAN, or a USB-link address --
/// meant a rebuild. This makes it a runtime setting instead: see
/// `screens/settings_screen.dart`.
class DaemonEndpointStore {
  static const _hostKey = 'daemon_host';
  static const _portKey = 'daemon_port';

  /// Returns the persisted endpoint, or null if none has been saved yet
  /// (first launch, or the platform's local storage is unavailable).
  Future<DaemonEndpoint?> load() async {
    final prefs = await SharedPreferences.getInstance();
    final host = prefs.getString(_hostKey);
    final port = prefs.getInt(_portKey);
    if (host == null || port == null) return null;
    return DaemonEndpoint(host: host, port: port);
  }

  Future<void> save(DaemonEndpoint endpoint) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_hostKey, endpoint.host);
    await prefs.setInt(_portKey, endpoint.port);
  }
}
