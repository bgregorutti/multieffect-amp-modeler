/// One block in a preset's signal chain.
///
/// Mirrors `control_daemon.models.EffectBlock`. The daemon treats `type` and
/// `params` as opaque, engine-defined data -- it has no built-in knowledge of
/// individual effect DSP internals (reverb, delay, ...), and neither does
/// this app. The UI must therefore present a generic editor (free-text type,
/// enabled toggle, generic key/value params list) rather than hardcoding a
/// fixed catalog of "known" effect types.
class EffectBlock {
  final String type;
  final bool enabled;

  /// Values are opaque, engine-defined data. The daemon's schema allows
  /// `float | int | str | bool` here (see `models.py: EffectBlock.params`),
  /// so this is `Map<String, Object?>` holding only those JSON-primitive
  /// types (never nested maps/lists).
  final Map<String, Object?> params;

  const EffectBlock({
    required this.type,
    this.enabled = true,
    this.params = const {},
  });

  factory EffectBlock.fromJson(Map<String, dynamic> json) {
    return EffectBlock(
      type: json['type'] as String,
      enabled: json['enabled'] as bool? ?? true,
      params: json['params'] == null
          ? const {}
          : Map<String, Object?>.from(json['params'] as Map),
    );
  }

  Map<String, dynamic> toJson() => {
        'type': type,
        'enabled': enabled,
        'params': params,
      };

  EffectBlock copyWith({
    String? type,
    bool? enabled,
    Map<String, Object?>? params,
  }) {
    return EffectBlock(
      type: type ?? this.type,
      enabled: enabled ?? this.enabled,
      params: params ?? this.params,
    );
  }
}
