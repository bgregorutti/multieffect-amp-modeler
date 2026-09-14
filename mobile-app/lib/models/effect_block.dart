/// One block in a rig's signal chain.
///
/// Mirrors `control_daemon.models.EffectBlock`. The daemon treats `type` and
/// `params` as opaque, engine-defined data -- it has no built-in knowledge of
/// individual effect DSP internals (reverb, delay, ...), and neither does
/// this app. The UI must therefore present a generic editor (free-text type,
/// enabled toggle, generic key/value params list) rather than hardcoding a
/// fixed catalog of "known" effect types.
///
/// [assetId] is how a block references the uploaded library: an amp block
/// points at a `.nam` asset, a cab block at an IR. Keeping the reference on
/// the block (rather than as special fields on the preset) is what makes
/// chain order explicit and allows more than one IR in a chain.
///
/// [pinned] marks a block as part of the rig's fixed backline -- always on,
/// never toggled by a preset. Amp and cab are the usual pinned blocks.
class EffectBlock {
  final String id;
  final String type;
  final String? assetId;
  final bool pinned;
  final bool enabled;

  /// Values are opaque, engine-defined data. The daemon's schema allows
  /// `float | int | str | bool` here (see `models.py: EffectBlock.params`),
  /// so this is `Map<String, Object?>` holding only those JSON-primitive
  /// types (never nested maps/lists).
  final Map<String, Object?> params;

  const EffectBlock({
    required this.id,
    required this.type,
    this.assetId,
    this.pinned = false,
    this.enabled = true,
    this.params = const {},
  });

  factory EffectBlock.fromJson(Map<String, dynamic> json) {
    return EffectBlock(
      id: json['id'] as String,
      type: json['type'] as String,
      assetId: json['asset_id'] as String?,
      pinned: json['pinned'] as bool? ?? false,
      enabled: json['enabled'] as bool? ?? true,
      params: json['params'] == null
          ? const {}
          : Map<String, Object?>.from(json['params'] as Map),
    );
  }

  Map<String, dynamic> toJson() => {
        'id': id,
        'type': type,
        'asset_id': assetId,
        'pinned': pinned,
        'enabled': enabled,
        'params': params,
      };

  EffectBlock copyWith({
    String? type,
    String? Function()? assetId,
    bool? pinned,
    bool? enabled,
    Map<String, Object?>? params,
  }) {
    return EffectBlock(
      id: id,
      type: type ?? this.type,
      assetId: assetId != null ? assetId() : this.assetId,
      pinned: pinned ?? this.pinned,
      enabled: enabled ?? this.enabled,
      params: params ?? this.params,
    );
  }
}
