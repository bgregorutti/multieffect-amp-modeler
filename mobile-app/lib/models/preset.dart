import 'effect_block.dart';

/// One preset's override of one of its rig's blocks.
///
/// Mirrors `control_daemon.models.PresetBlockState`. [params] is an override
/// map, not a full copy: keys absent here fall back to the block's own values
/// when the daemon resolves the chain. Per-preset parameter values are
/// supported by the wire format but not yet driven by this UI.
class PresetBlockState {
  final bool enabled;
  final Map<String, Object?> params;

  const PresetBlockState({
    this.enabled = true,
    this.params = const {},
  });

  factory PresetBlockState.fromJson(Map<String, dynamic> json) {
    return PresetBlockState(
      enabled: json['enabled'] as bool? ?? true,
      params: json['params'] == null
          ? const {}
          : Map<String, Object?>.from(json['params'] as Map),
    );
  }

  Map<String, dynamic> toJson() => {
        'enabled': enabled,
        'params': params,
      };

  PresetBlockState copyWith({bool? enabled, Map<String, Object?>? params}) {
    return PresetBlockState(
      enabled: enabled ?? this.enabled,
      params: params ?? this.params,
    );
  }
}

/// A set of on/off overrides within one rig.
///
/// Mirrors `control_daemon.models.Preset`. [blockStates] is keyed by
/// `EffectBlock.id`. A block with no entry keeps its own default `enabled`;
/// pinned blocks ignore any entry entirely and are always on, so the UI must
/// not offer to switch them.
class Preset {
  final String id;
  final String name;
  final Map<String, PresetBlockState> blockStates;
  final double createdAt;
  final double updatedAt;

  const Preset({
    required this.id,
    required this.name,
    this.blockStates = const {},
    required this.createdAt,
    required this.updatedAt,
  });

  factory Preset.fromJson(Map<String, dynamic> json) {
    final raw = json['block_states'] as Map<String, dynamic>? ?? const {};
    return Preset(
      id: json['id'] as String,
      name: json['name'] as String,
      blockStates: raw.map(
        (blockId, state) => MapEntry(
          blockId,
          PresetBlockState.fromJson(state as Map<String, dynamic>),
        ),
      ),
      createdAt: (json['created_at'] as num).toDouble(),
      updatedAt: (json['updated_at'] as num).toDouble(),
    );
  }

  Map<String, dynamic> toJson() => {
        'id': id,
        'name': name,
        'block_states':
            blockStates.map((blockId, s) => MapEntry(blockId, s.toJson())),
        'created_at': createdAt,
        'updated_at': updatedAt,
      };

  /// Whether [block] sounds in this preset. Pinned blocks are always on;
  /// otherwise a preset override wins, falling back to the block's own
  /// default. Mirrors `models.resolve_preset` on the daemon side.
  bool isBlockEnabled(EffectBlock block) {
    if (block.pinned) return true;
    return blockStates[block.id]?.enabled ?? block.enabled;
  }

  Preset copyWith({
    String? name,
    Map<String, PresetBlockState>? blockStates,
  }) {
    return Preset(
      id: id,
      name: name ?? this.name,
      blockStates: blockStates ?? this.blockStates,
      createdAt: createdAt,
      updatedAt: updatedAt,
    );
  }
}
