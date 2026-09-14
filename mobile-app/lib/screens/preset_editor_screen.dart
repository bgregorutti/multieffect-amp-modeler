import 'package:flutter/material.dart';

import '../models/preset.dart';
import '../models/rig.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';

/// Edits one preset: its name, and which of its rig's switchable blocks are
/// on. That is all a preset is -- the amp, the cab and the chain order belong
/// to the rig and are edited in `RigChainEditorScreen`.
///
/// Pinned blocks are shown read-only rather than hidden, so it is obvious
/// what is sounding underneath: they are part of what you hear, but no preset
/// can switch them off.
class PresetEditorScreen extends StatefulWidget {
  final DaemonStateController controller;
  final Rig rig;
  final Preset preset;

  const PresetEditorScreen({
    super.key,
    required this.controller,
    required this.rig,
    required this.preset,
  });

  @override
  State<PresetEditorScreen> createState() => _PresetEditorScreenState();
}

class _PresetEditorScreenState extends State<PresetEditorScreen> {
  late TextEditingController _nameController;
  late Map<String, bool> _enabledByBlockId;

  @override
  void initState() {
    super.initState();
    _nameController = TextEditingController(text: widget.preset.name);
    _enabledByBlockId = {
      for (final block in widget.rig.switchableBlocks)
        block.id: widget.preset.isBlockEnabled(block),
    };
  }

  @override
  void dispose() {
    _nameController.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    // Only switchable blocks get an entry; pinned blocks are always on and
    // the daemon ignores any state sent for them.
    final blockStates = {
      for (final entry in _enabledByBlockId.entries)
        entry.key: PresetBlockState(enabled: entry.value),
    };

    await widget.controller.client.updatePreset(
      UpdatePresetCommand(
        rigId: widget.rig.id,
        presetId: widget.preset.id,
        name: _nameController.text.trim(),
        blockStates: blockStates,
      ),
    );
    if (mounted) Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    final pinned = widget.rig.pinnedBlocks;
    final switchable = widget.rig.switchableBlocks;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Edit Preset'),
        actions: [
          IconButton(
            key: const Key('save-preset-button'),
            icon: const Icon(Icons.check),
            onPressed: _save,
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          TextField(
            key: const Key('preset-name-input'),
            controller: _nameController,
            decoration: const InputDecoration(labelText: 'Name'),
          ),
          const SizedBox(height: 24),
          Text('Always on', style: Theme.of(context).textTheme.titleMedium),
          Padding(
            padding: const EdgeInsets.only(top: 4, bottom: 8),
            child: Text(
              'Part of rig "${widget.rig.name}" -- shared by every preset in it.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          if (pinned.isEmpty)
            const ListTile(
              dense: true,
              title: Text('No pinned amp or cab in this rig'),
            )
          else
            for (final block in pinned)
              ListTile(
                key: Key('pinned-block-${block.id}'),
                dense: true,
                leading: const Icon(Icons.push_pin, size: 18),
                title: Text(block.type),
                subtitle: block.assetId == null
                    ? null
                    : Text(
                        widget.controller.state.assets[block.assetId]
                                ?.filename ??
                            block.assetId!,
                      ),
              ),
          const SizedBox(height: 24),
          Text('Effects', style: Theme.of(context).textTheme.titleMedium),
          if (switchable.isEmpty)
            const ListTile(
              dense: true,
              title: Text('No switchable effects in this rig'),
            )
          else
            for (final block in switchable)
              SwitchListTile(
                key: Key('preset-block-switch-${block.id}'),
                title: Text(block.type),
                value: _enabledByBlockId[block.id] ?? block.enabled,
                onChanged: (v) =>
                    setState(() => _enabledByBlockId[block.id] = v),
              ),
        ],
      ),
    );
  }
}
