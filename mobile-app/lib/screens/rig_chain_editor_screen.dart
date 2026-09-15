import 'dart:async';

import 'package:flutter/material.dart';

import '../models/asset.dart';
import '../models/block_param_descriptor.dart';
import '../models/effect_block.dart';
import '../models/rig.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';

/// The block types a rig's backline is made of. Effects (delay, eq, vst3,
/// ...) are added per preset in `PresetEditorScreen`, not here. A block
/// loaded with some other type is still shown as-is (see `_typeOptions`).
const List<String> _kBacklineTypes = ['gain', 'nam', 'ir', 'tone_stack', 'volume'];

/// The asset kind a block of [type] should be paired with, or null if it
/// isn't asset-backed (a native gain/eq/delay/etc. block, or an
/// unrecognized type gets no asset picker at all). A top-level function
/// (not a private method) so it's directly unit-testable without pumping
/// a widget tree -- this is the exact mapping that keeps a block's asset
/// picker from offering an asset of the wrong kind (e.g. a `.nam` file for
/// an `"ir"` block, which the engine rejects at load time with "missing
/// 'RIFF' chunk id").
AssetKind? assetKindForBlockType(String type) {
  switch (type) {
    case 'nam':
      return AssetKind.nam;
    case 'ir':
      return AssetKind.ir;
    case 'vst3':
      return AssetKind.vst3;
    default:
      return null;
  }
}

/// Edits a rig's name and its backline: the pinned blocks (input gain, amp,
/// cab, tone stack, output volume) shared by every preset. Per block: type,
/// asset (filtered to the kind that type wants) and default params.
///
/// Effects -- the unpinned blocks of the chain -- are managed from each
/// preset instead. They are not shown here, but are kept exactly where they
/// are in the chain when this screen saves.
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

  /// Static per-engine-build native block schema (same fetch/shape
  /// `PresetEditorScreen` already uses for its live-controls panel).
  List<BlockTypeDescriptor> _blockTypes = const [];

  @override
  void initState() {
    super.initState();
    _nameController = TextEditingController(text: widget.rig.name);
    _chain = List.of(widget.rig.chain);
    unawaited(_loadBlockTypes());
  }

  Future<void> _loadBlockTypes() async {
    final result =
        await widget.controller.client.listBlockTypes(const ListBlockTypesCommand());
    final raw = result['block_types'] as List<dynamic>? ?? const [];
    if (!mounted) return;
    setState(() {
      _blockTypes = raw
          .map((t) => BlockTypeDescriptor.fromJson(t as Map<String, dynamic>))
          .toList();
    });
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

  /// A brand-new backline block: pinned, and a "gain" so it always has a
  /// real schema.
  void _addBlock() {
    setState(() {
      _chain = [
        ..._chain,
        EffectBlock(
          id: _newBlockId(),
          type: 'gain',
          pinned: true,
          params: _defaultParamsFor('gain'),
        ),
      ];
    });
  }

  List<EffectBlock> get _backline => _chain.where((b) => b.pinned).toList();

  Map<String, Object?> _defaultParamsFor(String type) {
    for (final blockType in _blockTypes) {
      if (blockType.type == type) {
        return {for (final p in blockType.parameters) p.key: p.defaultValue};
      }
    }
    return const {};
  }

  void _removeBlock(String id) {
    setState(() => _chain = _chain.where((b) => b.id != id).toList());
  }

  /// Reorders backline blocks among themselves. The chain positions that
  /// hold backline blocks stay backline positions, so effects (not shown on
  /// this screen) keep their exact place in the chain.
  void _reorderBacklineBlocks(int oldIndex, int newIndex) {
    // `onReorderItem` (unlike the deprecated `onReorder`) already adjusts
    // `newIndex` for the removed item at `oldIndex`, so no manual `-1`
    // correction is needed here.
    setState(() {
      final backline = _backline;
      final item = backline.removeAt(oldIndex);
      backline.insert(newIndex, item);
      var i = 0;
      _chain = [for (final b in _chain) b.pinned ? backline[i++] : b];
    });
  }

  void _updateBlock(EffectBlock updated) {
    setState(() {
      _chain = [for (final b in _chain) b.id == updated.id ? updated : b];
    });
  }

  /// Type changed in the dropdown: the old asset (if any) almost certainly
  /// doesn't match the new type's kind, and the old params belong to the
  /// old type's schema -- both are reset rather than carried over stale.
  void _changeBlockType(EffectBlock current, String newType) {
    _updateBlock(
      current.copyWith(
        type: newType,
        assetId: () => null,
        params: _defaultParamsFor(newType),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final assets = widget.controller.state.assets.values.toList();
    final backline = _backline;

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
              Text('Backline',
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
              'Always on in every preset of this rig. '
              'Effects are added from each preset.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          ReorderableListView.builder(
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            itemCount: backline.length,
            onReorderItem: _reorderBacklineBlocks,
            itemBuilder: (context, index) {
              final block = backline[index];
              return _BlockEditor(
                key: ValueKey('block-editor-${block.id}'),
                index: index,
                block: block,
                assets: assets,
                blockTypes: _blockTypes,
                onChanged: _updateBlock,
                onTypeChanged: (t) => _changeBlockType(block, t),
                onRemove: () => _removeBlock(block.id),
              );
            },
          ),
        ],
      ),
    );
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
}

class _BlockEditor extends StatelessWidget {
  final int index;
  final EffectBlock block;
  final List<Asset> assets;
  final List<BlockTypeDescriptor> blockTypes;
  final ValueChanged<EffectBlock> onChanged;
  final ValueChanged<String> onTypeChanged;
  final VoidCallback onRemove;

  const _BlockEditor({
    super.key,
    required this.index,
    required this.block,
    required this.assets,
    required this.blockTypes,
    required this.onChanged,
    required this.onTypeChanged,
    required this.onRemove,
  });

  /// The backline types, plus this block's own current type if it is
  /// something else (e.g. a rig where tone_stack was turned into a delay),
  /// so the dropdown still shows it and it can be changed back.
  List<String> get _typeOptions => [
        ..._kBacklineTypes,
        if (!_kBacklineTypes.contains(block.type)) block.type,
      ];

  AssetKind? get _wantedAssetKind => assetKindForBlockType(block.type);

  /// The parameter schema for this block, if any -- a native type's schema
  /// from `list_block_types`, or a `vst3` block's own asset-specific schema
  /// (see `Asset.parameters`). Same lookup `PresetEditorScreen` uses for its
  /// live-controls panel, reused here to drive the *default*-params editor.
  List<BlockParamDescriptor> get _paramSchema {
    if (block.type == 'vst3') {
      final assetId = block.assetId;
      if (assetId == null) return const [];
      for (final asset in assets) {
        if (asset.id == assetId) return asset.parameters ?? const [];
      }
      return const [];
    }
    for (final blockType in blockTypes) {
      if (blockType.type == block.type) return blockType.parameters;
    }
    return const [];
  }

  @override
  Widget build(BuildContext context) {
    final wantedKind = _wantedAssetKind;
    final needsAsset = wantedKind != null;
    final schema = _paramSchema;
    // Asset-backed types have no params of their own; any other type with
    // no known schema falls back to the free-form editor so nothing becomes
    // uneditable.
    final isKnownType =
        needsAsset || blockTypes.any((t) => t.type == block.type);

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
                  child: DropdownButtonFormField<String>(
                    key: Key('block-type-field-$index'),
                    initialValue: block.type,
                    decoration: const InputDecoration(labelText: 'Type'),
                    items: [
                      for (final type in _typeOptions)
                        DropdownMenuItem(value: type, child: Text(type)),
                    ],
                    onChanged: (v) {
                      if (v != null && v != block.type) onTypeChanged(v);
                    },
                  ),
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
            if (needsAsset)
              _AssetDropdown(
                // Keyed by type too: a nam->ir change must start a fresh
                // field, not keep the old nam asset selected in a list that
                // no longer contains it.
                key: Key('block-asset-dropdown-$index-${block.type}'),
                label: 'Asset (${wantedKind.name.toUpperCase()})',
                assets: assets.where((a) => a.kind == wantedKind).toList(),
                value: block.assetId,
                onChanged: (id) => onChanged(block.copyWith(assetId: () => id)),
              ),
            if (schema.isNotEmpty)
              _SchemaParamsEditor(
                // Same reason: fields from the previous type must not keep
                // their typed text after params were reset to new defaults.
                key: ValueKey('schema-params-${block.type}'),
                descriptors: schema,
                params: block.params,
                onChanged: (p) => onChanged(block.copyWith(params: p)),
              )
            else if (!isKnownType)
              _ParamsEditor(
                params: block.params,
                onChanged: (p) => onChanged(block.copyWith(params: p)),
              )
            else if (block.type == 'vst3' && block.assetId != null)
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: Text(
                  'This plugin exposes no adjustable parameters.',
                  style: Theme.of(context).textTheme.bodySmall,
                ),
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
            child: Text(a.filename, overflow: TextOverflow.ellipsis),
          ),
        ),
      ],
      onChanged: onChanged,
    );
  }
}

/// One labeled numeric field per known parameter (label/unit from the
/// schema, current value from `params` falling back to the descriptor's own
/// default) -- the schema-driven replacement for `_ParamsEditor` when a
/// block's type (or, for `vst3`, its selected asset) has a real parameter
/// schema. No "add param" here: every field this block can have is already
/// shown.
class _SchemaParamsEditor extends StatelessWidget {
  final List<BlockParamDescriptor> descriptors;
  final Map<String, Object?> params;
  final ValueChanged<Map<String, Object?>> onChanged;

  const _SchemaParamsEditor({
    super.key,
    required this.descriptors,
    required this.params,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        for (final d in descriptors)
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: TextFormField(
              key: Key('param-value-field-${d.key}'),
              initialValue:
                  ((params[d.key] as num?)?.toDouble() ?? d.defaultValue).toString(),
              decoration: InputDecoration(
                labelText: d.unit.isEmpty ? d.label : '${d.label} (${d.unit})',
              ),
              keyboardType: const TextInputType.numberWithOptions(
                  decimal: true, signed: true),
              onChanged: (v) {
                final parsed = double.tryParse(v);
                if (parsed == null) return;
                final next = Map<String, Object?>.of(params);
                next[d.key] = parsed;
                onChanged(next);
              },
            ),
          ),
      ],
    );
  }
}

/// A generic key/value editor for a block's opaque `params` map -- the
/// fallback for a type this app has no known schema for (see
/// `_SchemaParamsEditor` for the normal case). Values are edited as free
/// text and coerced to `num`/`bool`/`String` (matching the daemon's
/// `float | int | str | bool` schema) on change.
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
