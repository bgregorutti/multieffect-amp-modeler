import 'package:flutter/material.dart';

import 'services/asset_upload_service.dart';
import 'services/daemon_client.dart';
import 'services/daemon_endpoint_store.dart';
import 'state/daemon_state_controller.dart';
import 'screens/assets_screen.dart';
import 'screens/rig_list_screen.dart';
import 'screens/connection_screen.dart';
import 'screens/footswitch_mapping_screen.dart';
import 'screens/preset_list_screen.dart';

/// Fallback daemon address used the first time the app runs, before the
/// user has saved anything in the Settings screen -- still overridable at
/// build time the same way it always was:
///
///     flutter build apk --dart-define=DAEMON_HOST=192.168.1.42
///
/// The 127.0.0.1 default only works when the app and the daemon run on the
/// same machine -- true for `flutter run -d chrome`/`-d web-server` on a dev
/// box, but never on a phone, where 127.0.0.1 is the phone itself.
///
/// Once the app has been run at least once, [DaemonEndpointStore] takes
/// over: the Settings screen (gear icon on the Status tab) lets the daemon's
/// address be changed at runtime -- e.g. when switching between the pedal's
/// Wi-Fi AP, a shared LAN, and a USB-link address -- without a rebuild.
const kDefaultDaemonHost =
    String.fromEnvironment('DAEMON_HOST', defaultValue: '127.0.0.1');
const kDefaultDaemonPort =
    int.fromEnvironment('DAEMON_PORT', defaultValue: 8765);

void main() {
  runApp(MultiEffectApp());
}

class MultiEffectApp extends StatefulWidget {
  /// Overridable for tests; defaults to the real shared_preferences-backed
  /// store.
  final DaemonEndpointStore endpointStore;

  MultiEffectApp({super.key, DaemonEndpointStore? endpointStore})
      : endpointStore = endpointStore ?? DaemonEndpointStore();

  @override
  State<MultiEffectApp> createState() => _MultiEffectAppState();
}

class _MultiEffectAppState extends State<MultiEffectApp> {
  DaemonEndpoint _endpoint =
      const DaemonEndpoint(host: kDefaultDaemonHost, port: kDefaultDaemonPort);

  late DaemonClientBase _daemonClient;
  late DaemonStateController _controller;
  late AssetUploadService _uploadService;

  @override
  void initState() {
    super.initState();
    _buildServices(connect: true);
    _loadPersistedEndpoint();
  }

  /// (Re)builds the client/controller/upload-service trio for [_endpoint].
  /// Called once from [initState] with the compile-time default, and again
  /// from [_applyEndpoint] whenever the user saves a new one in Settings.
  void _buildServices({required bool connect}) {
    _daemonClient = DaemonClient(uri: _endpoint.wsUri);
    _controller = DaemonStateController(_daemonClient);
    _uploadService =
        AssetUploadService(daemonHttpBaseUrl: _endpoint.httpBaseUrl);
    if (connect) _controller.connect();
  }

  /// Loads whatever endpoint was last saved to disk, if any, and swaps to
  /// it -- overriding the compile-time default this state started
  /// connecting to in [initState]. A no-op on first-ever launch.
  Future<void> _loadPersistedEndpoint() async {
    final stored = await widget.endpointStore.load();
    if (stored != null && stored != _endpoint) {
      _applyEndpoint(stored);
    }
  }

  /// Tears down the current client/controller/upload-service and rebuilds
  /// them against [endpoint], then reconnects. Used both by the initial
  /// load above and by the Settings screen's save callback.
  void _applyEndpoint(DaemonEndpoint endpoint) {
    _controller.dispose();
    _daemonClient.dispose();
    _uploadService.dispose();
    setState(() {
      _endpoint = endpoint;
      _buildServices(connect: true);
    });
  }

  Future<void> _saveEndpoint(DaemonEndpoint endpoint) async {
    await widget.endpointStore.save(endpoint);
    _applyEndpoint(endpoint);
  }

  @override
  void dispose() {
    _controller.dispose();
    _daemonClient.dispose();
    _uploadService.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Multi-Effect Amp Modeler',
      theme: ThemeData(colorSchemeSeed: Colors.deepPurple, useMaterial3: true),
      darkTheme: ThemeData(
        colorSchemeSeed: Colors.deepPurple,
        brightness: Brightness.dark,
        useMaterial3: true,
      ),
      home: HomeShell(
        controller: _controller,
        uploadService: _uploadService,
        currentEndpoint: _endpoint,
        onSaveEndpoint: _saveEndpoint,
      ),
    );
  }
}

/// Bottom-navigation shell holding the five top-level screens.
class HomeShell extends StatefulWidget {
  final DaemonStateController controller;
  final AssetUploadService uploadService;
  final DaemonEndpoint currentEndpoint;
  final ValueChanged<DaemonEndpoint> onSaveEndpoint;

  const HomeShell({
    super.key,
    required this.controller,
    required this.uploadService,
    required this.currentEndpoint,
    required this.onSaveEndpoint,
  });

  @override
  State<HomeShell> createState() => _HomeShellState();
}

class _HomeShellState extends State<HomeShell> {
  int _index = 0;

  /// Stub file picker: a real build wires this to `file_picker`, which
  /// needs platform channels unavailable in this headless environment. See
  /// the README's "what's stubbed" section.
  Future<PickedFile?> _stubFilePicker(String kind) async {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(
          'File picking for "$kind" is not wired up on this platform yet.',
        ),
      ),
    );
    return null;
  }

  @override
  Widget build(BuildContext context) {
    final screens = [
      ConnectionScreen(
        controller: widget.controller,
        currentEndpoint: widget.currentEndpoint,
        onSaveEndpoint: widget.onSaveEndpoint,
      ),
      RigListScreen(controller: widget.controller),
      PresetListScreen(controller: widget.controller),
      FootswitchMappingScreen(controller: widget.controller),
      AssetsScreen(
        controller: widget.controller,
        uploadService: widget.uploadService,
        pickFile: _stubFilePicker,
      ),
    ];

    return Scaffold(
      body: IndexedStack(index: _index, children: screens),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _index,
        onDestinationSelected: (i) => setState(() => _index = i),
        destinations: const [
          NavigationDestination(
              icon: Icon(Icons.monitor_heart), label: 'Status'),
          NavigationDestination(icon: Icon(Icons.speaker), label: 'Rigs'),
          NavigationDestination(icon: Icon(Icons.piano), label: 'Presets'),
          NavigationDestination(
              icon: Icon(Icons.settings_input_component), label: 'Footswitch'),
          NavigationDestination(
              icon: Icon(Icons.folder_open), label: 'Assets'),
        ],
      ),
    );
  }
}
