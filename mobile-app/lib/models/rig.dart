import 'effect_block.dart';
import 'preset.dart';

/// One backline: an ordered signal chain plus the presets that toggle it.
///
/// Mirrors `control_daemon.models.Rig`. The chain's pinned blocks (amp, cab)
/// stay on across every preset in this rig; the rest are what presets switch.
/// Rig order in `DaemonState.rigs` is the order the rig up/down footswitches
/// step through.
///
/// The split matters for real-time behaviour, not just tidiness: changing
/// preset within a rig never reloads a NAM model or re-partitions an IR, so
/// it is fast and click-free. Changing rig does, which is why that is the
/// between-songs gesture.
class Rig {
  final String id;
  final String name;
  final List<EffectBlock> chain;
  final List<Preset> presets;

  const Rig({
    required this.id,
    required this.name,
    this.chain = const [],
    this.presets = const [],
  });

  factory Rig.fromJson(Map<String, dynamic> json) {
    return Rig(
      id: json['id'] as String,
      name: json['name'] as String,
      chain: (json['chain'] as List<dynamic>? ?? const [])
          .map((b) => EffectBlock.fromJson(b as Map<String, dynamic>))
          .toList(),
      presets: (json['presets'] as List<dynamic>? ?? const [])
          .map((p) => Preset.fromJson(p as Map<String, dynamic>))
          .toList(),
    );
  }

  Map<String, dynamic> toJson() => {
        'id': id,
        'name': name,
        'chain': chain.map((b) => b.toJson()).toList(),
        'presets': presets.map((p) => p.toJson()).toList(),
      };

  /// The blocks a preset is allowed to switch -- everything but the pinned
  /// amp/cab backline.
  List<EffectBlock> get switchableBlocks =>
      chain.where((b) => !b.pinned).toList();

  /// The fixed backline: always on, shared by every preset in this rig.
  List<EffectBlock> get pinnedBlocks => chain.where((b) => b.pinned).toList();

  Rig copyWith({
    String? name,
    List<EffectBlock>? chain,
    List<Preset>? presets,
  }) {
    return Rig(
      id: id,
      name: name ?? this.name,
      chain: chain ?? this.chain,
      presets: presets ?? this.presets,
    );
  }
}
