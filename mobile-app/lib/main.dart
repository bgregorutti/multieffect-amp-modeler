import 'package:flutter/material.dart';

import 'services/asset_upload_service.dart';
import 'services/daemon_client.dart';
import 'state/daemon_state_controller.dart';
import 'screens/assets_screen.dart';
import 'screens/rig_list_screen.dart';
import 'screens/connection_screen.dart';
import 'screens/footswitch_mapping_screen.dart';
import 'screens/preset_list_screen.dart';

/// Default daemon location. Real deployments will want this configurable
/// (e.g. a settings screen backed by shared_preferences) -- out of scope for
/// this first pass, called out in the README.
const kDefaultDaemonHost = '127.0.0.1';
const kDefaultDaemonPort = 8765;

void main() {
  runApp(MultiEffectApp(
    daemonClient: DaemonClient(
      uri: Uri.parse('ws://$kDefaultDaemonHost:$kDefaultDaemonPort/ws'),
    ),
  ));
}

class MultiEffectApp extends StatefulWidget {
  final DaemonClientBase daemonClient;

  const MultiEffectApp({super.key, required this.daemonClient});

  @override
  State<MultiEffectApp> createState() => _MultiEffectAppState();
}

class _MultiEffectAppState extends State<MultiEffectApp> {
  late final DaemonStateController _controller;
  late final AssetUploadService _uploadService;

  @override
  void initState() {
    super.initState();
    _controller = DaemonStateController(widget.daemonClient);
    _uploadService = AssetUploadService(
      daemonHttpBaseUrl:
          Uri.parse('http://$kDefaultDaemonHost:$kDefaultDaemonPort'),
    );
    _controller.connect();
  }

  @override
  void dispose() {
    _controller.dispose();
    widget.daemonClient.dispose();
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
      home: HomeShell(controller: _controller, uploadService: _uploadService),
    );
  }
}

/// Bottom-navigation shell holding the five top-level screens.
class HomeShell extends StatefulWidget {
  final DaemonStateController controller;
  final AssetUploadService uploadService;

  const HomeShell({
    super.key,
    required this.controller,
    required this.uploadService,
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
      ConnectionScreen(controller: widget.controller),
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
