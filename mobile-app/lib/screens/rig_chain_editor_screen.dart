import 'package:flutter/material.dart';

import '../models/asset.dart';
import '../models/effect_block.dart';
import '../models/rig.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';

/// Edits a rig's name and its signal `chain`: add/remove/reorder blocks, and
/// per block set `type` (free text), `asset_id` (from the uploaded library),
/// `pinned`, `enabled` and `params` (generic key-value editor).
///
/// `EffectBlock.type`/`params` are opaque, engine-defined data as far as the
/// daemon (and this app) are concerned -- this is deliberately a generic
/// editor, not a bespoke UI per "known" effect type.
///
/// Pinning is the rig/preset split made visible: a pinned block (the amp and
/// cab) is always on and cannot be switched by any preset in this rig, so the
/// preset editor only offers the unpinned ones.
class RigChainEditorScreen extends StatefulWidget {
  final DaemonStateController controller;
  final Rig rig;

  const RigChainEditorScreen({
    super.key,
    required this.controller,
    required this.rig,
  });

  @override
  State<RigChainEditorScreen> createState() => _RigChainEditorScreenState();
}

class _RigChainEditorScreenState extends State<RigChainEditorScreen> {
  late TextEditingController _nameController;
  late List<EffectBlock> _chain;
  int _newBlockCounter = 0;

  @override
  void initState() {
    super.initState();
    _nameController = TextEditingController(text: widget.rig.name);
    _chain = List.of(widget.rig.chain);
  }

  @override
  void dispose() {
    _nameController.dispose();
    super.dispose();
  }

  /// Blocks need stable ids because a rig's presets reference them by id.
  /// The daemon would assign one if we omitted it, but generating it here
  /// keeps the id stable across edits made before the first save.
  String _newBlockId() {
    _newBlockCounter++;
    return 'blk-${DateTime.now().microsecondsSinceEpoch}-$_newBlockCounter';
  }

  void _addBlock() {
    setState(() {
      _chain = [..._chain, EffectBlock(id: _newBlockId(), type: 'new_block')];
    });
  }

  void _removeBlockAt(int index) {
    setState(() {
      final next = List.of(_chain);
      next.removeAt(index);
      _chain = next;
    });
  }

  void _reorderBlocks(int oldIndex, int newIndex) {
    // `onReorderItem` (unlike the deprecated `onReorder`) already adjusts
    // `newIndex` for the removed item at `oldIndex`, so no manual `-1`
    // correction is needed here.
    setState(() {
      final next = List.of(_chain);
      final item = next.removeAt(oldIndex);
      next.insert(newIndex, item);
      _chain = next;
    });
  }

  void _updateBlockAt(int index, EffectBlock updated) {
    setState(() {
      final next = List.of(_chain);
      next[index] = updated;
      _chain = next;
    });
  }

  Future<void> _save() async {
    await widget.controller.client.updateRig(
      UpdateRigCommand(
        rigId: widget.rig.id,
        name: _nameController.text.trim(),
        chain: _chain,
      ),
    );
    if (mounted) Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    final assets = widget.controller.state.assets.values.toList();

    return Scaffold(
      appBar: AppBar(
        title: const Text('Edit Rig'),
        actions: [
          IconButton(
            key: const Key('save-rig-button'),
            icon: const Icon(Icons.check),
            onPressed: _save,
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          TextField(
            key: const Key('rig-name-input'),
            controller: _nameController,
            decoration: const InputDecoration(labelText: 'Name'),
          ),
          const SizedBox(height: 24),
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text('Signal chain',
                  style: Theme.of(context).textTheme.titleMedium),
              IconButton(
                key: const Key('add-block-button'),
                icon: const Icon(Icons.add),
                onPressed: _addBlock,
              ),
            ],
          ),
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: Text(
              'Pinned blocks (amp, cab) stay on in every preset. '
              'Unpinned blocks are what presets switch.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          ReorderableListView.builder(
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            itemCount: _chain.length,
            onReorderItem: _reorderBlocks,
            itemBuilder: (context, index) {
              return _BlockEditor(
                key: ValueKey('block-editor-${_chain[index].id}'),
                index: index,
                block: _chain[index],
                assets: assets,
                onChanged: (b) => _updateBlockAt(index, b),
                onRemove: () => _removeBlockAt(index),
              );
            },
          ),
        ],
      ),
    );
  }
}

class _BlockEditor extends StatelessWidget {
  final int index;
  final EffectBlock block;
  final List<Asset> assets;
  final ValueChanged<EffectBlock> onChanged;
  final VoidCallback onRemove;

  const _BlockEditor({
    super.key,
    required this.index,
    required this.block,
    required this.assets,
    required this.onChanged,
    required this.onRemove,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      key: ValueKey('block-card-$index'),
      margin: const EdgeInsets.symmetric(vertical: 4),
      child: Padding(
        padding: const EdgeInsets.all(8.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Expanded(
                  child: TextFormField(
                    key: Key('block-type-field-$index'),
                    initialValue: block.type,
                    decoration: const InputDecoration(labelText: 'Type'),
                    onChanged: (v) => onChanged(block.copyWith(type: v)),
                  ),
                ),
                Switch(
                  key: Key('block-enabled-switch-$index'),
                  value: block.enabled,
                  onChanged: (v) => onChanged(block.copyWith(enabled: v)),
                ),
                IconButton(
                  key: Key('remove-block-button-$index'),
                  icon: const Icon(Icons.delete_outline),
                  onPressed: onRemove,
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
            _AssetDropdown(
              key: Key('block-asset-dropdown-$index'),
              label: 'Asset (NAM / IR)',
              assets: assets,
              value: block.assetId,
              onChanged: (id) => onChanged(block.copyWith(assetId: () => id)),
            ),
            SwitchListTile(
              key: Key('block-pinned-switch-$index'),
              contentPadding: EdgeInsets.zero,
              dense: true,
              title: const Text('Pinned (always on)'),
              value: block.pinned,
              onChanged: (v) => onChanged(block.copyWith(pinned: v)),
            ),
            _ParamsEditor(
              params: block.params,
              onChanged: (p) => onChanged(block.copyWith(params: p)),
            ),
          ],
        ),
      ),
    );
  }
}

class _AssetDropdown extends StatelessWidget {
  final String label;
  final List<Asset> assets;
  final String? value;
  final ValueChanged<String?> onChanged;

  const _AssetDropdown({
    super.key,
    required this.label,
    required this.assets,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final validValue = assets.any((a) => a.id == value) ? value : null;
    return DropdownButtonFormField<String?>(
      initialValue: validValue,
      decoration: InputDecoration(labelText: label),
      items: [
        const DropdownMenuItem<String?>(value: null, child: Text('None')),
        ...assets.map(
          (a) => DropdownMenuItem<String?>(
            value: a.id,
            child: Text('${a.filename} (${a.kind.toWire()})'),
          ),
        ),
      ],
      onChanged: onChanged,
    );
  }
}

/// A generic key/value editor for a block's opaque `params` map. Values are
/// edited as free text and coerced to `num`/`bool`/`String` (matching the
/// daemon's `float | int | str | bool` schema) on change.
class _ParamsEditor extends StatelessWidget {
  final Map<String, Object?> params;
  final ValueChanged<Map<String, Object?>> onChanged;

  const _ParamsEditor({required this.params, required this.onChanged});

  Object? _coerce(String raw) {
    if (raw == 'true') return true;
    if (raw == 'false') return false;
    final n = num.tryParse(raw);
    if (n != null) return n;
    return raw;
  }

  @override
  Widget build(BuildContext context) {
    final entries = params.entries.toList();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        for (final entry in entries)
          Row(
            key: Key('param-row-${entry.key}'),
            children: [
              Expanded(
                child: Text(entry.key, overflow: TextOverflow.ellipsis),
              ),
              Expanded(
                child: TextFormField(
                  key: Key('param-value-field-${entry.key}'),
                  initialValue: entry.value?.toString() ?? '',
                  onChanged: (v) {
                    final next = Map<String, Object?>.of(params);
                    next[entry.key] = _coerce(v);
                    onChanged(next);
                  },
                ),
              ),
              IconButton(
                key: Key('remove-param-button-${entry.key}'),
                icon: const Icon(Icons.close, size: 18),
                onPressed: () {
                  final next = Map<String, Object?>.of(params);
                  next.remove(entry.key);
                  onChanged(next);
                },
              ),
            ],
          ),
        TextButton.icon(
          key: const Key('add-param-button'),
          icon: const Icon(Icons.add, size: 18),
          label: const Text('Add param'),
          onPressed: () {
            var newKey = 'param';
            var i = 1;
            while (params.containsKey(newKey)) {
              newKey = 'param$i';
              i++;
            }
            final next = Map<String, Object?>.of(params);
            next[newKey] = 0;
            onChanged(next);
          },
        ),
      ],
    );
  }
}
