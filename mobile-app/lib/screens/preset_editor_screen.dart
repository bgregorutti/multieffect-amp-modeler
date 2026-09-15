import 'dart:async';

import 'package:flutter/material.dart';

import '../models/asset.dart';
import '../models/block_param_descriptor.dart';
import '../models/effect_block.dart';
import '../models/preset.dart';
import '../models/rig.dart';
import '../models/ws_messages.dart';
import '../services/daemon_client.dart';
import '../state/daemon_state_controller.dart';

/// Native block types that belong to the rig's backline rather than being
/// offered as a preset effect (see `RigChainEditorScreen`).
const _kBacklineOnlyTypes = {'volume', 'tone_stack'};

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

  int _newEffectCounter = 0;

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

  /// Effect types offered by "Add effect": every native type except the
  /// backline-only ones, plus vst3 when a plugin has been registered.
  List<String> get _effectTypes => [
        for (final t in _blockTypes)
          if (!_kBacklineOnlyTypes.contains(t.type)) t.type,
        if (widget.controller.state.assets.values
            .any((a) => a.kind == AssetKind.vst3))
          'vst3',
      ];

  Future<void> _addEffect() async {
    final type = await showDialog<String>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Add effect'),
        children: [
          for (final t in _effectTypes)
            SimpleDialogOption(
              key: Key('add-effect-type-$t'),
              onPressed: () => Navigator.of(context).pop(t),
              child: Text(t),
            ),
        ],
      ),
    );
    if (type == null || !mounted) return;

    String? assetId;
    var params = <String, Object?>{};
    if (type == 'vst3') {
      final plugins = widget.controller.state.assets.values
          .where((a) => a.kind == AssetKind.vst3)
          .toList();
      assetId = await showDialog<String>(
        context: context,
        builder: (context) => SimpleDialog(
          title: const Text('Choose plugin'),
          children: [
            for (final a in plugins)
              SimpleDialogOption(
                onPressed: () => Navigator.of(context).pop(a.id),
                child: Text(a.filename),
              ),
          ],
        ),
      );
      if (assetId == null || !mounted) return;
    } else {
      for (final t in _blockTypes) {
        if (t.type == type) {
          params = {for (final p in t.parameters) p.key: p.defaultValue};
        }
      }
    }

    _newEffectCounter++;
    final effect = EffectBlock(
      id: 'fx-$type-${DateTime.now().microsecondsSinceEpoch}-$_newEffectCounter',
      type: type,
      assetId: assetId,
      // Off at rig level: presets with no override of their own stay off.
      enabled: false,
      params: params,
    );

    final rig = _rig;
    final nextRig = rig.copyWith(chain: rig.chainWithEffect(effect));
    try {
      await widget.controller.client.updateRig(
        UpdateRigCommand(rigId: rig.id, chain: nextRig.chain),
      );
    } on DaemonCommandError catch (e) {
      _showError(e);
      return;
    }
    if (!mounted) return;
    setState(() => _enabledByBlockId[effect.id] = true);
    await _sendBlockStates(nextRig);
  }

  Future<void> _removeEffect(EffectBlock block) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Remove ${block.type}?'),
        content: Text(
          'This removes it from every preset of rig "${_rig.name}". '
          'To just turn it off here, use its switch instead.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('Cancel'),
          ),
          TextButton(
            key: const Key('confirm-remove-effect'),
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('Remove'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    final rig = _rig;
    try {
      await widget.controller.client.updateRig(UpdateRigCommand(
        rigId: rig.id,
        chain: rig.chain.where((b) => b.id != block.id).toList(),
      ));
    } on DaemonCommandError catch (e) {
      _showError(e);
    }
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

  Widget _buildScaffold(BuildContext context) {
    final rig = _rig;
    final pinned = rig.pinnedBlocks;
    final switchable = rig.switchableBlocks;

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
              'Part of rig "${rig.name}" -- shared by every preset in it.',
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
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text('Effects', style: Theme.of(context).textTheme.titleMedium),
              TextButton.icon(
                key: const Key('add-effect-button'),
                icon: const Icon(Icons.add),
                label: const Text('Add effect'),
                onPressed: _effectTypes.isEmpty ? null : _addEffect,
              ),
            ],
          ),
          if (switchable.isEmpty)
            const ListTile(
              dense: true,
              title: Text('No effects yet'),
            )
          else
            for (final block in switchable)
              Row(
                children: [
                  Expanded(
                    child: SwitchListTile(
                      key: Key('preset-block-switch-${block.id}'),
                      title: Text(block.type),
                      subtitle: block.assetId == null
                          ? null
                          : Text(
                              widget.controller.state.assets[block.assetId]
                                      ?.filename ??
                                  block.assetId!,
                            ),
                      value: _isOn(block),
                      onChanged: (v) => _setEffectEnabled(block, v),
                    ),
                  ),
                  IconButton(
                    key: Key('remove-effect-${block.id}'),
                    icon: const Icon(Icons.delete_outline),
                    onPressed: () => _removeEffect(block),
                  ),
                ],
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
