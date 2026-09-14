import 'package:flutter/material.dart';

import '../models/preset.dart';
import '../models/rig.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';
import 'preset_editor_screen.dart';

/// Lists the presets of the *active rig* -- the things the left/right
/// footswitches step through. Supports create/rename/delete and tapping a
/// preset to select it on the pedal.
///
/// Presets are scoped to a rig by design: a preset only records which of its
/// own rig's blocks are on, so it is meaningless against any other rig. To
/// see another rig's presets, make that rig active on the Rigs screen.
class PresetListScreen extends StatelessWidget {
  final DaemonStateController controller;

  const PresetListScreen({super.key, required this.controller});

  Future<void> _createPreset(BuildContext context, Rig rig) async {
    final name = await _promptForName(context, title: 'New preset', initial: '');
    if (name == null || name.isEmpty) return;
    await controller.client
        .createPreset(CreatePresetCommand(rigId: rig.id, name: name));
  }

  Future<void> _renamePreset(
      BuildContext context, Rig rig, Preset preset) async {
    final name = await _promptForName(
      context,
      title: 'Rename preset',
      initial: preset.name,
    );
    if (name == null || name.isEmpty) return;
    await controller.client.updatePreset(
      UpdatePresetCommand(rigId: rig.id, presetId: preset.id, name: name),
    );
  }

  Future<void> _deletePreset(Rig rig, Preset preset) async {
    await controller.client.deletePreset(
      DeletePresetCommand(rigId: rig.id, presetId: preset.id),
    );
  }

  Future<void> _selectPreset(int index) async {
    await controller.client
        .selectPreset(SelectPresetCommand(presetIndex: index));
  }

  Future<String?> _promptForName(
    BuildContext context, {
    required String title,
    required String initial,
  }) {
    final textController = TextEditingController(text: initial);
    return showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(title),
        content: TextField(
          key: const Key('preset-name-field'),
          controller: textController,
          autofocus: true,
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(),
            child: const Text('Cancel'),
          ),
          TextButton(
            key: const Key('preset-name-save'),
            onPressed: () =>
                Navigator.of(context).pop(textController.text.trim()),
            child: const Text('Save'),
          ),
        ],
      ),
    );
  }

  /// A short summary of which of the rig's switchable blocks this preset
  /// turns on -- the whole point of a preset, so it belongs on the tile.
  String _effectsSummary(Rig rig, Preset preset) {
    final on = rig.switchableBlocks
        .where((b) => preset.isBlockEnabled(b))
        .map((b) => b.type)
        .toList();
    return on.isEmpty ? 'no effects on' : on.join(' + ');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Presets'),
        actions: [
          ListenableBuilder(
            listenable: controller,
            builder: (context, _) {
              final rig = controller.state.activeRig;
              return IconButton(
                key: const Key('add-preset-button'),
                icon: const Icon(Icons.add),
                onPressed:
                    rig == null ? null : () => _createPreset(context, rig),
              );
            },
          ),
        ],
      ),
      body: ListenableBuilder(
        listenable: controller,
        builder: (context, _) {
          final rig = controller.state.activeRig;
          if (rig == null) {
            return const Center(
              child: Text('No rig selected -- pick one on the Rigs screen'),
            );
          }

          final presets = rig.presets;
          if (presets.isEmpty) {
            return Center(child: Text('No presets in "${rig.name}" yet'));
          }

          final activeIndex = controller.state.activePresetIndex;

          return Column(
            children: [
              ListTile(
                dense: true,
                leading: const Icon(Icons.speaker),
                title: Text(rig.name),
                subtitle: Text(
                  rig.pinnedBlocks.isEmpty
                      ? 'no pinned amp/cab'
                      : rig.pinnedBlocks.map((b) => b.type).join(' + '),
                ),
              ),
              const Divider(height: 1),
              Expanded(
                child: ListView.builder(
                  itemCount: presets.length,
                  itemBuilder: (context, index) {
                    final preset = presets[index];
                    final isActive = index == activeIndex;
                    return ListTile(
                      key: Key('preset-tile-${preset.id}'),
                      leading: Icon(
                        isActive ? Icons.check_circle : Icons.piano,
                        color: isActive ? Colors.green : null,
                      ),
                      title: Text(preset.name),
                      subtitle: Text(_effectsSummary(rig, preset)),
                      onTap: () => _selectPreset(index),
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
                                    rig: rig,
                                    preset: preset,
                                  ),
                                ),
                              );
                            },
                          ),
                          IconButton(
                            key: Key('rename-preset-${preset.id}'),
                            icon: const Icon(Icons.drive_file_rename_outline),
                            onPressed: () =>
                                _renamePreset(context, rig, preset),
                          ),
                          IconButton(
                            key: Key('delete-preset-${preset.id}'),
                            icon: const Icon(Icons.delete),
                            onPressed: () => _deletePreset(rig, preset),
                          ),
                        ],
                      ),
                    );
                  },
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}
