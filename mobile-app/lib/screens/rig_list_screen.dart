import 'package:flutter/material.dart';

import '../models/rig.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';
import 'rig_chain_editor_screen.dart';

/// Lists every rig in order, which is the order the rig up/down footswitches
/// step through. Supports create/rename/delete/reorder, editing a rig's
/// signal chain, and tapping a rig to make it active on the pedal.
///
/// Selecting a rig is the expensive switch: it swaps the whole backline, so
/// the engine reloads its NAM model and IR. Switching presets within a rig
/// (see `PresetListScreen`) does not.
class RigListScreen extends StatelessWidget {
  final DaemonStateController controller;

  const RigListScreen({super.key, required this.controller});

  Future<void> _createRig(BuildContext context) async {
    final name = await _promptForName(context, title: 'New rig', initial: '');
    if (name == null || name.isEmpty) return;
    await controller.client.createRig(CreateRigCommand(name: name));
  }

  Future<void> _renameRig(BuildContext context, Rig rig) async {
    final name = await _promptForName(
      context,
      title: 'Rename rig',
      initial: rig.name,
    );
    if (name == null || name.isEmpty) return;
    await controller.client.updateRig(
      UpdateRigCommand(rigId: rig.id, name: name),
    );
  }

  Future<void> _deleteRig(Rig rig) async {
    await controller.client.deleteRig(DeleteRigCommand(rigId: rig.id));
  }

  Future<void> _selectRig(int index) async {
    await controller.client
        .selectPreset(SelectPresetCommand(rigIndex: index, presetIndex: 0));
  }

  Future<void> _reorder(List<Rig> rigs, int oldIndex, int newIndex) async {
    final next = List.of(rigs);
    final item = next.removeAt(oldIndex);
    next.insert(newIndex, item);
    await controller.client.reorderRigs(
      ReorderRigsCommand(rigIds: next.map((r) => r.id).toList()),
    );
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
          key: const Key('rig-name-field'),
          controller: textController,
          autofocus: true,
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(),
            child: const Text('Cancel'),
          ),
          TextButton(
            key: const Key('rig-name-save'),
            onPressed: () =>
                Navigator.of(context).pop(textController.text.trim()),
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
        title: const Text('Rigs'),
        actions: [
          IconButton(
            key: const Key('add-rig-button'),
            icon: const Icon(Icons.add),
            onPressed: () => _createRig(context),
          ),
        ],
      ),
      body: ListenableBuilder(
        listenable: controller,
        builder: (context, _) {
          final rigs = controller.state.rigs;
          final activeIndex = controller.state.activeRigIndex;

          if (rigs.isEmpty) {
            return const Center(child: Text('No rigs yet'));
          }

          return ReorderableListView.builder(
            itemCount: rigs.length,
            onReorderItem: (oldIndex, newIndex) =>
                _reorder(rigs, oldIndex, newIndex),
            itemBuilder: (context, index) {
              final rig = rigs[index];
              final isActive = index == activeIndex;
              final pinned = rig.pinnedBlocks.map((b) => b.type).join(' + ');

              return ListTile(
                key: Key('rig-tile-${rig.id}'),
                leading: Icon(
                  isActive ? Icons.check_circle : Icons.speaker,
                  color: isActive ? Colors.green : null,
                ),
                title: Text(rig.name),
                subtitle: Text(
                  '${rig.presets.length} presets  ·  '
                  '${rig.switchableBlocks.length} effects'
                  '${pinned.isEmpty ? '' : '  ·  $pinned'}',
                ),
                onTap: () => _selectRig(index),
                trailing: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    IconButton(
                      key: Key('edit-rig-${rig.id}'),
                      icon: const Icon(Icons.tune),
                      onPressed: () {
                        Navigator.of(context).push(
                          MaterialPageRoute(
                            builder: (_) => RigChainEditorScreen(
                              controller: controller,
                              rig: rig,
                            ),
                          ),
                        );
                      },
                    ),
                    IconButton(
                      key: Key('rename-rig-${rig.id}'),
                      icon: const Icon(Icons.drive_file_rename_outline),
                      onPressed: () => _renameRig(context, rig),
                    ),
                    IconButton(
                      key: Key('delete-rig-${rig.id}'),
                      icon: const Icon(Icons.delete),
                      onPressed: () => _deleteRig(rig),
                    ),
                    ReorderableDragStartListener(
                      index: index,
                      child: const Padding(
                        padding: EdgeInsets.symmetric(horizontal: 4),
                        child: Icon(Icons.drag_handle),
                      ),
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
