import 'dart:async';

import 'package:flutter/material.dart';

import '../models/block_param_descriptor.dart';
import '../models/effect_block.dart';
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

  /// Static per-engine-build native block schema, fetched once when this
  /// screen opens (see `list_block_types` -- it's engine-build state, not
  /// per-rig state, so there's no need to keep it fresh beyond that).
  List<BlockTypeDescriptor> _blockTypes = const [];

  /// The live-controls panel's own view of each block's current param
  /// values -- seeded from the preset's overrides (falling back to the
  /// rig block's own default, then the descriptor's default) and updated
  /// locally on every slider drag so the UI responds immediately rather
  /// than waiting for the round-trip broadcast.
  final Map<String, Map<String, double>> _liveValues = {};

  @override
  void initState() {
    super.initState();
    _nameController = TextEditingController(text: widget.preset.name);
    _enabledByBlockId = {
      for (final block in widget.rig.switchableBlocks)
        block.id: widget.preset.isBlockEnabled(block),
    };
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

  /// The parameter schema for [block], if any: a native type's schema from
  /// `list_block_types`, or a `vst3` block's own asset-specific schema (see
  /// `Asset.parameters`). Empty for a block with no known schema at all
  /// (an experimental/unrecognized type) -- that block simply gets no live
  /// controls section, same "fail safe" spirit as elsewhere in this app.
  List<BlockParamDescriptor> _descriptorsFor(EffectBlock block) {
    if (block.type == 'vst3') {
      final assetId = block.assetId;
      if (assetId == null) return const [];
      return widget.controller.state.assets[assetId]?.parameters ?? const [];
    }
    for (final blockType in _blockTypes) {
      if (blockType.type == block.type) return blockType.parameters;
    }
    return const [];
  }

  double _resolvedValue(EffectBlock block, BlockParamDescriptor descriptor) {
    final live = _liveValues[block.id]?[descriptor.key];
    if (live != null) return live;
    final override = widget.preset.blockStates[block.id]?.params[descriptor.key];
    final fromBlock = block.params[descriptor.key];
    final raw = override ?? fromBlock;
    if (raw is num) return raw.toDouble();
    return descriptor.defaultValue;
  }

  void _onLiveParamChanged(
      EffectBlock block, BlockParamDescriptor descriptor, double value) {
    setState(() {
      _liveValues.putIfAbsent(block.id, () => {})[descriptor.key] = value;
    });
    // Fire-and-forget, same "live, not gated on Save" pattern as every
    // other slider in this app -- a dropped reply here just means the next
    // drag tick (or a reconnect's fresh state) supersedes it.
    unawaited(widget.controller.client.setBlockParam(SetBlockParamCommand(
      rigId: widget.rig.id,
      presetId: widget.preset.id,
      blockId: block.id,
      paramKey: descriptor.key,
      value: value,
    )));
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
          const SizedBox(height: 24),
          Text('Live controls', style: Theme.of(context).textTheme.titleMedium),
          Padding(
            padding: const EdgeInsets.only(top: 4, bottom: 8),
            child: Text(
              'Adjusts this preset only, and takes effect immediately -- '
              'no need to save.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          for (final block in widget.rig.chain)
            if (_descriptorsFor(block).isNotEmpty)
              _LiveParamsCard(
                key: Key('live-params-card-${block.id}'),
                block: block,
                descriptors: _descriptorsFor(block),
                valueOf: (descriptor) => _resolvedValue(block, descriptor),
                onChanged: (descriptor, value) =>
                    _onLiveParamChanged(block, descriptor, value),
              ),
        ],
      ),
    );
  }
}

/// One block's live-adjustable parameters, rendered from its
/// [BlockParamDescriptor] schema -- a real `Slider` per parameter (bound to
/// its own min/max/unit), never a generic key/value editor. Discrete
/// parameters (`stepCount > 0`) get a `Slider` with matching `divisions` so
/// the thumb snaps to real steps, rather than a free continuous drag.
class _LiveParamsCard extends StatelessWidget {
  final EffectBlock block;
  final List<BlockParamDescriptor> descriptors;
  final double Function(BlockParamDescriptor) valueOf;
  final void Function(BlockParamDescriptor, double) onChanged;

  const _LiveParamsCard({
    super.key,
    required this.block,
    required this.descriptors,
    required this.valueOf,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: 4),
      child: Padding(
        padding: const EdgeInsets.all(8.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(block.type, style: Theme.of(context).textTheme.titleSmall),
            for (final descriptor in descriptors)
              _LiveParamSlider(
                key: Key('live-param-${block.id}-${descriptor.key}'),
                descriptor: descriptor,
                value: valueOf(descriptor).clamp(descriptor.min, descriptor.max),
                onChanged: (v) => onChanged(descriptor, v),
              ),
          ],
        ),
      ),
    );
  }
}

class _LiveParamSlider extends StatelessWidget {
  final BlockParamDescriptor descriptor;
  final double value;
  final ValueChanged<double> onChanged;

  const _LiveParamSlider({
    super.key,
    required this.descriptor,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final unitSuffix = descriptor.unit.isEmpty ? '' : ' ${descriptor.unit}';
    return Row(
      children: [
        SizedBox(
          width: 72,
          child: Text(descriptor.label, overflow: TextOverflow.ellipsis),
        ),
        Expanded(
          child: Slider(
            min: descriptor.min,
            max: descriptor.max,
            divisions: descriptor.stepCount > 0 ? descriptor.stepCount : null,
            value: value,
            onChanged: onChanged,
          ),
        ),
        SizedBox(
          width: 64,
          child: Text(
            '${value.toStringAsFixed(2)}$unitSuffix',
            textAlign: TextAlign.end,
            style: Theme.of(context).textTheme.bodySmall,
          ),
        ),
      ],
    );
  }
}
