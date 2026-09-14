import 'package:flutter/material.dart';

import '../services/daemon_client.dart';
import '../state/daemon_state_controller.dart';

/// The phone-side mirror of what an onboard display would show: connection
/// state, the active rig and preset, bypass, and tempo.
class ConnectionScreen extends StatelessWidget {
  final DaemonStateController controller;

  const ConnectionScreen({super.key, required this.controller});

  String _statusLabel(ConnectionStatus status) {
    switch (status) {
      case ConnectionStatus.disconnected:
        return 'Disconnected';
      case ConnectionStatus.connecting:
        return 'Connecting…';
      case ConnectionStatus.connected:
        return 'Connected';
      case ConnectionStatus.error:
        return 'Connection error';
    }
  }

  Color _statusColor(ConnectionStatus status, BuildContext context) {
    switch (status) {
      case ConnectionStatus.connected:
        return Colors.green;
      case ConnectionStatus.connecting:
        return Colors.orange;
      case ConnectionStatus.disconnected:
        return Colors.grey;
      case ConnectionStatus.error:
        return Colors.red;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Pedal Status')),
      body: ListenableBuilder(
        listenable: controller,
        builder: (context, _) {
          final state = controller.state;
          final status = controller.status;
          final activeRig = state.activeRig;
          final activePreset = state.activePreset;

          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              Card(
                child: ListTile(
                  leading: Icon(Icons.circle,
                      color: _statusColor(status, context), size: 16),
                  title: Text(_statusLabel(status)),
                  subtitle: const Text('Connection to control daemon'),
                  trailing: status == ConnectionStatus.connected
                      ? null
                      : TextButton(
                          key: const Key('connect-button'),
                          onPressed: () => controller.connect(),
                          child: const Text('Connect'),
                        ),
                ),
              ),
              const SizedBox(height: 12),
              Card(
                child: ListTile(
                  key: const Key('active-rig-tile'),
                  leading: const Icon(Icons.speaker),
                  title: Text(activeRig?.name ?? 'No rig selected'),
                  subtitle: Text(
                    activeRig == null
                        ? 'up / down switches step through rigs'
                        : activeRig.pinnedBlocks.isEmpty
                            ? 'no pinned amp/cab'
                            : activeRig.pinnedBlocks
                                .map((b) => b.type)
                                .join(' + '),
                  ),
                ),
              ),
              const SizedBox(height: 12),
              Card(
                child: ListTile(
                  key: const Key('active-preset-tile'),
                  leading: const Icon(Icons.piano),
                  title: Text(activePreset?.name ?? 'No preset selected'),
                  subtitle: Text(
                    activeRig == null || activePreset == null
                        ? 'left / right switches step through presets'
                        : 'Preset ${state.activePresetIndex + 1} of '
                            '${activeRig.presets.length}',
                  ),
                ),
              ),
              const SizedBox(height: 12),
              Card(
                child: SwitchListTile(
                  key: const Key('bypass-switch'),
                  title: const Text('Bypass'),
                  value: state.bypass,
                  onChanged: null,
                ),
              ),
              const SizedBox(height: 12),
              Card(
                child: ListTile(
                  key: const Key('tempo-tile'),
                  leading: const Icon(Icons.speed),
                  title: Text(
                    state.tempoBpm != null
                        ? '${state.tempoBpm!.toStringAsFixed(1)} BPM'
                        : 'No tempo set',
                  ),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}
