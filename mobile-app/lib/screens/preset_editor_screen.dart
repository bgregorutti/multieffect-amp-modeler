import 'package:flutter/material.dart';

import '../models/asset.dart';
import '../models/effect_block.dart';
import '../models/preset.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';

/// Edits a preset's name and its `blocks` list: add/remove/reorder blocks,
/// edit each block's `type` (free text)/`enabled`/`params` (generic
/// key-value editor), and assign `nam_asset_id`/`ir_asset_id` from the asset
/// library.
///
/// `EffectBlock.type`/`params` are opaque, engine-defined data as far as the
/// daemon (and this app) are concerned -- this is deliberately a generic
/// editor, not a bespoke UI per "known" effect type.
class PresetEditorScreen extends StatefulWidget {
  final DaemonStateController controller;
  final Preset preset;

  const PresetEditorScreen({
    super.key,
    required this.controller,
    required this.preset,
  });

  @override
  State<PresetEditorScreen> createState() => _PresetEditorScreenState();
}

class _PresetEditorScreenState extends State<PresetEditorScreen> {
  late TextEditingController _nameController;
  late List<EffectBlock> _blocks;
  String? _namAssetId;
  String? _irAssetId;

  @override
  void initState() {
    super.initState();
    _nameController = TextEditingController(text: widget.preset.name);
    _blocks = List.of(widget.preset.blocks);
    _namAssetId = widget.preset.namAssetId;
    _irAssetId = widget.preset.irAssetId;
  }

  @override
  void dispose() {
    _nameController.dispose();
    super.dispose();
  }

  void _addBlock() {
    setState(() {
      _blocks = [..._blocks, const EffectBlock(type: 'new_block')];
    });
  }

  void _removeBlockAt(int index) {
    setState(() {
      final next = List.of(_blocks);
      next.removeAt(index);
      _blocks = next;
    });
  }

  void _reorderBlocks(int oldIndex, int newIndex) {
    // `onReorderItem` (unlike the deprecated `onReorder`) already adjusts
    // `newIndex` for the removed item at `oldIndex`, so no manual `-1`
    // correction is needed here.
    setState(() {
      final next = List.of(_blocks);
      final item = next.removeAt(oldIndex);
      next.insert(newIndex, item);
      _blocks = next;
    });
  }

  void _updateBlockAt(int index, EffectBlock updated) {
    setState(() {
      final next = List.of(_blocks);
      next[index] = updated;
      _blocks = next;
    });
  }

  Future<void> _save() async {
    await widget.controller.client.updatePreset(
      UpdatePresetCommand(
        presetId: widget.preset.id,
        name: _nameController.text.trim(),
        blocks: _blocks,
        namAssetId: () => _namAssetId,
        irAssetId: () => _irAssetId,
      ),
    );
    if (mounted) Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    final assets = widget.controller.state.assets.values;
    final namAssets = assets.where((a) => a.kind == AssetKind.nam).toList();
    final irAssets = assets.where((a) => a.kind == AssetKind.ir).toList();

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
          const SizedBox(height: 16),
          _AssetDropdown(
            key: const Key('nam-asset-dropdown'),
            label: 'NAM model',
            assets: namAssets,
            value: _namAssetId,
            onChanged: (id) => setState(() => _namAssetId = id),
          ),
          const SizedBox(height: 8),
          _AssetDropdown(
            key: const Key('ir-asset-dropdown'),
            label: 'IR file',
            assets: irAssets,
            value: _irAssetId,
            onChanged: (id) => setState(() => _irAssetId = id),
          ),
          const SizedBox(height: 24),
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text('Blocks', style: Theme.of(context).textTheme.titleMedium),
              IconButton(
                key: const Key('add-block-button'),
                icon: const Icon(Icons.add),
                onPressed: _addBlock,
              ),
            ],
          ),
          ReorderableListView.builder(
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            itemCount: _blocks.length,
            onReorderItem: _reorderBlocks,
            itemBuilder: (context, index) {
              return _BlockEditor(
                key: ValueKey('block-editor-$index'),
                index: index,
                block: _blocks[index],
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
    final validValue =
        assets.any((a) => a.id == value) ? value : null;
    return DropdownButtonFormField<String?>(
      initialValue: validValue,
      decoration: InputDecoration(labelText: label),
      items: [
        const DropdownMenuItem<String?>(value: null, child: Text('None')),
        ...assets.map(
          (a) => DropdownMenuItem<String?>(value: a.id, child: Text(a.filename)),
        ),
      ],
      onChanged: onChanged,
    );
  }
}

class _BlockEditor extends StatelessWidget {
  final int index;
  final EffectBlock block;
  final ValueChanged<EffectBlock> onChanged;
  final VoidCallback onRemove;

  const _BlockEditor({
    super.key,
    required this.index,
    required this.block,
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
