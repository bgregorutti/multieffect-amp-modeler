import 'package:flutter/material.dart';

import '../models/preset.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';
import 'preset_editor_screen.dart';

/// Lists all presets; supports create/rename/delete, and tapping a preset to
/// select it on the pedal (`select_preset`).
class PresetListScreen extends StatelessWidget {
  final DaemonStateController controller;

  const PresetListScreen({super.key, required this.controller});

  Future<void> _createPreset(BuildContext context) async {
    final name = await _promptForName(context, title: 'New preset', initial: '');
    if (name == null || name.isEmpty) return;
    await controller.client.createPreset(CreatePresetCommand(name: name));
  }

  Future<void> _renamePreset(BuildContext context, Preset preset) async {
    final name = await _promptForName(
      context,
      title: 'Rename preset',
      initial: preset.name,
    );
    if (name == null || name.isEmpty) return;
    await controller.client.updatePreset(
      UpdatePresetCommand(presetId: preset.id, name: name),
    );
  }

  Future<void> _deletePreset(BuildContext context, Preset preset) async {
    await controller.client
        .deletePreset(DeletePresetCommand(presetId: preset.id));
  }

  Future<void> _selectPreset(Preset preset) async {
    await controller.client.selectPreset(SelectPresetCommand.byId(preset.id));
  }

  Future<String?> _promptForName(
    BuildContext context, {
    required String title,
    required String initial,
  }) {
    final controller = TextEditingController(text: initial);
    return showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(title),
        content: TextField(
          key: const Key('preset-name-field'),
          controller: controller,
          autofocus: true,
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(),
            child: const Text('Cancel'),
          ),
          TextButton(
            key: const Key('preset-name-save'),
            onPressed: () => Navigator.of(context).pop(controller.text.trim()),
            child: const Text('Save'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Presets'),
        actions: [
          IconButton(
            key: const Key('add-preset-button'),
            icon: const Icon(Icons.add),
            onPressed: () => _createPreset(context),
          ),
        ],
      ),
      body: ListenableBuilder(
        listenable: controller,
        builder: (context, _) {
          final presets = controller.state.presets.values.toList()
            ..sort((a, b) => a.name.compareTo(b.name));
          final activeId = controller.state.activePresetId;

          if (presets.isEmpty) {
            return const Center(child: Text('No presets yet'));
          }

          return ListView.builder(
            itemCount: presets.length,
            itemBuilder: (context, index) {
              final preset = presets[index];
              return ListTile(
                key: Key('preset-tile-${preset.id}'),
                leading: Icon(
                  preset.id == activeId ? Icons.check_circle : Icons.piano,
                  color: preset.id == activeId ? Colors.green : null,
                ),
                title: Text(preset.name),
                subtitle: Text('${preset.blocks.length} blocks'),
                onTap: () => _selectPreset(preset),
                trailing: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    IconButton(
                      key: Key('edit-preset-${preset.id}'),
                      icon: const Icon(Icons.edit),
                      onPressed: () {
                        Navigator.of(context).push(
                          MaterialPageRoute(
                            builder: (_) => PresetEditorScreen(
                              controller: controller,
                              preset: preset,
                            ),
                          ),
                        );
                      },
                    ),
                    IconButton(
                      key: Key('rename-preset-${preset.id}'),
                      icon: const Icon(Icons.drive_file_rename_outline),
                      onPressed: () => _renamePreset(context, preset),
                    ),
                    IconButton(
                      key: Key('delete-preset-${preset.id}'),
                      icon: const Icon(Icons.delete),
                      onPressed: () => _deletePreset(context, preset),
                    ),
                  ],
                ),
              );
            },
          );
        },
      ),
    );
  }
}
