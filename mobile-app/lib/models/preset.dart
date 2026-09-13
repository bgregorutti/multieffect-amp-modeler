import 'effect_block.dart';

/// Mirrors `control_daemon.models.Preset`.
class Preset {
  final String id;
  final String name;
  final List<EffectBlock> blocks;
  final String? namAssetId;
  final String? irAssetId;
  final double createdAt;
  final double updatedAt;

  const Preset({
    required this.id,
    required this.name,
    this.blocks = const [],
    this.namAssetId,
    this.irAssetId,
    required this.createdAt,
    required this.updatedAt,
  });

  factory Preset.fromJson(Map<String, dynamic> json) {
    return Preset(
      id: json['id'] as String,
      name: json['name'] as String,
      blocks: (json['blocks'] as List<dynamic>? ?? const [])
          .map((b) => EffectBlock.fromJson(b as Map<String, dynamic>))
          .toList(),
      namAssetId: json['nam_asset_id'] as String?,
      irAssetId: json['ir_asset_id'] as String?,
      createdAt: (json['created_at'] as num).toDouble(),
      updatedAt: (json['updated_at'] as num).toDouble(),
    );
  }

  Map<String, dynamic> toJson() => {
        'id': id,
        'name': name,
        'blocks': blocks.map((b) => b.toJson()).toList(),
        'nam_asset_id': namAssetId,
        'ir_asset_id': irAssetId,
        'created_at': createdAt,
        'updated_at': updatedAt,
      };

  Preset copyWith({
    String? name,
    List<EffectBlock>? blocks,
    String? Function()? namAssetId,
    String? Function()? irAssetId,
  }) {
    return Preset(
      id: id,
      name: name ?? this.name,
      blocks: blocks ?? this.blocks,
      namAssetId: namAssetId != null ? namAssetId() : this.namAssetId,
      irAssetId: irAssetId != null ? irAssetId() : this.irAssetId,
      createdAt: createdAt,
      updatedAt: updatedAt,
    );
  }
}
