import 'package:flutter/material.dart';

import '../models/ws_messages.dart';
import '../services/daemon_client.dart';
import '../services/daemon_endpoint_store.dart';
import '../state/daemon_state_controller.dart';
import 'settings_screen.dart';

/// The phone-side mirror of what an onboard display would show: connection
/// state, the active rig and preset, bypass, and tempo. Also the entry
/// point to the Settings screen, since "where's my daemon" belongs next to
/// "am I connected to it".
class ConnectionScreen extends StatelessWidget {
  final DaemonStateController controller;
  final DaemonEndpoint currentEndpoint;
  final ValueChanged<DaemonEndpoint> onSaveEndpoint;

  const ConnectionScreen({
    super.key,
    required this.controller,
    required this.currentEndpoint,
    required this.onSaveEndpoint,
  });

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

  /// Asks first: both cut the sound, and powering off needs someone at the
  /// pedal to bring it back.
  Future<void> _requestPower(BuildContext context, SystemPowerCommand cmd) async {
    final reboot = cmd.action == 'reboot';
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(reboot ? 'Restart the pedal?' : 'Shut down the pedal?'),
        content: Text(reboot
            ? 'The sound stops for about a minute while the pedal restarts. '
                'The app reconnects by itself.'
            : 'The sound stops, and the pedal stays off until its power '
                'switch is used again. Wait for the screen to go dark before '
                'unplugging it.'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('Cancel'),
          ),
          FilledButton(
            key: const Key('confirm-power-button'),
            onPressed: () => Navigator.of(context).pop(true),
            child: Text(reboot ? 'Restart' : 'Shut down'),
          ),
        ],
      ),
    );
    if (confirmed != true || !context.mounted) return;

    String message;
    try {
      await controller.client.systemPower(cmd);
      message = reboot ? 'Restarting the pedal…' : 'Shutting down the pedal…';
    } on DaemonCommandError catch (e) {
      message = e.message;
    }
    if (!context.mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(message),
      duration: const Duration(seconds: 6),
    ));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Pedal Status'),
        actions: [
          IconButton(
            key: const Key('open-settings-button'),
            icon: const Icon(Icons.settings),
            tooltip: 'Daemon connection settings',
            onPressed: () {
              Navigator.of(context).push(MaterialPageRoute(
                builder: (context) => SettingsScreen(
                  currentEndpoint: currentEndpoint,
                  onSave: onSaveEndpoint,
                ),
              ));
            },
          ),
        ],
      ),
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
              const SizedBox(height: 12),
              Card(
                child: Padding(
                  padding: const EdgeInsets.all(12),
                  child: Row(
                    children: [
                      Expanded(
                        child: OutlinedButton.icon(
                          key: const Key('reboot-button'),
                          icon: const Icon(Icons.restart_alt),
                          label: const Text('Restart'),
                          onPressed: status == ConnectionStatus.connected
                              ? () => _requestPower(
                                  context, const SystemPowerCommand.reboot())
                              : null,
                        ),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: OutlinedButton.icon(
                          key: const Key('poweroff-button'),
                          icon: const Icon(Icons.power_settings_new),
                          label: const Text('Shut down'),
                          onPressed: status == ConnectionStatus.connected
                              ? () => _requestPower(
                                  context, const SystemPowerCommand.powerOff())
                              : null,
                        ),
                      ),
                    ],
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
