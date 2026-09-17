import 'dart:async';

import 'package:flutter/material.dart';

import '../models/asset_category.dart';
import '../models/block_param_descriptor.dart';
import '../models/effect_block.dart';
import '../models/preset.dart';
import '../models/rig.dart';
import '../models/ws_messages.dart';
import '../services/daemon_client.dart';
import '../state/daemon_state_controller.dart';
import '../widgets/chain_connector.dart';
import '../widgets/chain_stage_card.dart';

/// Edits one preset: its name, which effects it uses, and their live
/// parameters.
///
/// Effects are added here, not in the rig editor. Under the hood an effect
/// is still an unpinned block in the rig's chain (that is what the daemon
/// and engine understand); adding one inserts it before the cab, off by
/// default at rig level so every *other* preset is unaffected, and turns it
/// on in this preset.
///
/// Effect switches and sliders apply immediately. Every block_states update
/// is merged from the latest daemon state, because the daemon's
/// `update_preset` replaces block_states wholesale -- sending only the
/// switches would wipe the parameter overrides the sliders store there.
/// Save only renames.
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

  /// Switch positions changed on this screen, applied before the resulting
  /// broadcast arrives so the switch doesn't flicker back.
  final Map<String, bool> _enabledByBlockId = {};

  /// Static per-engine-build native block schema, fetched once when this
  /// screen opens.
  List<BlockTypeDescriptor> _blockTypes = const [];

  /// Slider values dragged on this screen, so the UI responds immediately
  /// rather than waiting for the round-trip broadcast.
  final Map<String, Map<String, double>> _liveValues = {};


  @override
  void initState() {
    super.initState();
    _nameController = TextEditingController(text: widget.preset.name);
    unawaited(_loadBlockTypes());
  }

  @override
  void dispose() {
    _nameController.dispose();
    super.dispose();
  }

  /// The rig and preset as the daemon currently has them -- the constructor
  /// arguments are only a snapshot from navigation time, and adding or
  /// removing an effect changes the chain while this screen is open.
  Rig get _rig => widget.controller.state.rigById(widget.rig.id) ?? widget.rig;

  Preset get _preset {
    for (final p in _rig.presets) {
      if (p.id == widget.preset.id) return p;
    }
    return widget.preset;
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

  bool _isOn(EffectBlock block) =>
      block.pinned ||
      (_enabledByBlockId[block.id] ?? _preset.isBlockEnabled(block));

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
    final override = _preset.blockStates[block.id]?.params[descriptor.key];
    final fromBlock = block.params[descriptor.key];
    final raw = override ?? fromBlock;
    if (raw is num) return raw.toDouble();
    return descriptor.defaultValue;
  }

  /// True when the value `_resolvedValue` returned for this block+key came
  /// from *this preset's own override* -- either dragged on this screen
  /// (not yet round-tripped through a broadcast) or already persisted in
  /// `preset.blockStates[block.id].params` -- rather than from the block's
  /// own default. Drives the small "overridden" dot on `_LiveParamSlider`
  /// (see the issue's open question: a live override otherwise looks
  /// identical to a default value).
  bool _isOverridden(EffectBlock block, BlockParamDescriptor descriptor) {
    if (_liveValues[block.id]?[descriptor.key] != null) return true;
    return _preset.blockStates[block.id]?.params[descriptor.key] != null;
  }

  /// This preset's full block_states for [rig]'s switchable blocks: latest
  /// daemon overrides (params included), plus this screen's pending switch
  /// and slider changes on top.
  Map<String, PresetBlockState> _mergedBlockStates(Rig rig) {
    final current = _preset.blockStates;
    return {
      for (final block in rig.switchableBlocks)
        block.id: PresetBlockState(
          enabled: _enabledByBlockId[block.id] ??
              current[block.id]?.enabled ??
              block.enabled,
          params: {
            ...?current[block.id]?.params,
            ...?_liveValues[block.id],
          },
        ),
    };
  }

  Future<void> _sendBlockStates(Rig rig) async {
    try {
      await widget.controller.client.updatePreset(UpdatePresetCommand(
        rigId: rig.id,
        presetId: widget.preset.id,
        blockStates: _mergedBlockStates(rig),
      ));
    } on DaemonCommandError catch (e) {
      _showError(e);
    }
  }

  void _setEffectEnabled(EffectBlock block, bool enabled) {
    setState(() => _enabledByBlockId[block.id] = enabled);
    unawaited(_sendBlockStates(_rig));
  }

  void _onLiveParamChanged(
      EffectBlock block, BlockParamDescriptor descriptor, double value) {
    setState(() {
      _liveValues.putIfAbsent(block.id, () => {})[descriptor.key] = value;
    });
    unawaited(widget.controller.client.setBlockParam(SetBlockParamCommand(
      rigId: widget.rig.id,
      presetId: widget.preset.id,
      blockId: block.id,
      paramKey: descriptor.key,
      value: value,
    )));
  }

  void _showError(DaemonCommandError e) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text(e.message)));
  }

  Future<void> _save() async {
    await widget.controller.client.updatePreset(
      UpdatePresetCommand(
        rigId: widget.rig.id,
        presetId: widget.preset.id,
        name: _nameController.text.trim(),
      ),
    );
    if (mounted) Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: widget.controller,
      builder: (context, _) => _buildScaffold(context),
    );
  }

  /// One card in the "Signal chain" strip: the locked/greyed variant for a
  /// rig-inherited pinned block (no tap -- matches the existing "no switch
  /// for pinned blocks" rule, just a nicer visual treatment), or the normal
  /// interactive variant for a switchable effect, with its enable switch
  /// and remove button reachable directly on the card (same `Key`s the
  /// vertical layout used to expose, so nothing in the daemon-facing
  /// behavior changes -- only how it's laid out).
  Widget _chainStageCardFor(BuildContext context, EffectBlock block) {
    final asset =
        block.assetId == null ? null : widget.controller.state.assets[block.assetId];
    if (block.pinned) {
      return ChainStageCard(
        key: Key('pinned-block-${block.id}'),
        icon: blockTypeIcon(block.type),
        label: block.type,
        subtitle: asset?.displayLabel,
        isLocked: true,
      );
    }
    return ChainStageCard(
      key: Key('switchable-block-${block.id}'),
      width: 156,
      icon: blockTypeIcon(block.type),
      label: block.type,
      subtitle: asset?.displayLabel,
      footer: SwitchListTile(
        key: Key('preset-block-switch-${block.id}'),
        dense: true,
        contentPadding: EdgeInsets.zero,
        visualDensity: VisualDensity.compact,
        value: _isOn(block),
        onChanged: (v) => _setEffectEnabled(block, v),
      ),
    );
  }

  Widget _buildScaffold(BuildContext context) {
    final rig = _rig;

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
          Text('Signal chain', style: Theme.of(context).textTheme.titleMedium),
          Padding(
            padding: const EdgeInsets.only(top: 4, bottom: 8),
            child: Text(
              'The board is rig "${rig.name}" and is the same for every '
              'preset in it. Here you choose which pedals are on, and how '
              'they are set. To add or remove one, edit the rig.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          if (rig.chain.isEmpty)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 8),
              child: Text('This rig has no blocks yet.'),
            )
          else
            SizedBox(
              height: 150,
              child: ListView(
                scrollDirection: Axis.horizontal,
                children: [
                  for (var i = 0; i < rig.chain.length; i++) ...[
                    _chainStageCardFor(context, rig.chain[i]),
                    if (i < rig.chain.length - 1) const ChainConnector(),
                  ],
                ],
              ),
            ),
          const SizedBox(height: 24),
          Text('Live controls', style: Theme.of(context).textTheme.titleMedium),
          Padding(
            padding: const EdgeInsets.only(top: 4, bottom: 8),
            child: Text(
              'Adjusts this preset only, and takes effect immediately -- '
              'no need to save. Effects that are off here have no controls.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          for (final block in rig.chain)
            if (_isOn(block) && _descriptorsFor(block).isNotEmpty)
              _LiveParamsCard(
                key: Key('live-params-card-${block.id}'),
                block: block,
                descriptors: _descriptorsFor(block),
                valueOf: (descriptor) => _resolvedValue(block, descriptor),
                isOverridden: (descriptor) =>
                    _isOverridden(block, descriptor),
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
  final bool Function(BlockParamDescriptor) isOverridden;
  final void Function(BlockParamDescriptor, double) onChanged;

  const _LiveParamsCard({
    super.key,
    required this.block,
    required this.descriptors,
    required this.valueOf,
    required this.isOverridden,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: 4),
      elevation: 0,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(
          color: Theme.of(context).colorScheme.outlineVariant.withValues(alpha: 0.4),
        ),
      ),
      child: Padding(
        padding: const EdgeInsets.all(12.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(block.type, style: Theme.of(context).textTheme.titleSmall),
            for (final descriptor in descriptors)
              _LiveParamSlider(
                key: Key('live-param-${block.id}-${descriptor.key}'),
                descriptor: descriptor,
                value: valueOf(descriptor).clamp(descriptor.min, descriptor.max),
                isOverridden: isOverridden(descriptor),
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
  final bool isOverridden;
  final ValueChanged<double> onChanged;

  const _LiveParamSlider({
    super.key,
    required this.descriptor,
    required this.value,
    required this.isOverridden,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final unitSuffix = descriptor.unit.isEmpty ? '' : ' ${descriptor.unit}';
    return Row(
      children: [
        // A small, unobtrusive dot -- not a badge or banner -- marking that
        // this preset has its own value for this parameter rather than
        // inheriting the block's default. See the issue's open question:
        // previously a live override had no visual distinction at all.
        SizedBox(
          width: 10,
          child: isOverridden
              ? Tooltip(
                  message: 'This preset overrides the default value',
                  child: Container(
                    key: const Key('live-param-override-dot'),
                    width: 6,
                    height: 6,
                    decoration: BoxDecoration(
                      color: Theme.of(context).colorScheme.tertiary,
                      shape: BoxShape.circle,
                    ),
                  ),
                )
              : null,
        ),
        SizedBox(
          width: 68,
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
