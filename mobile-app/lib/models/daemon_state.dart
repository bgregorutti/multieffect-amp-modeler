import 'asset.dart';
import 'footswitch_action.dart';
import 'preset.dart';
import 'rig.dart';

/// The full persisted + in-memory state of the daemon.
///
/// Mirrors `control_daemon.models.DaemonState`. This is what arrives inside
/// every `state_snapshot`/`state_changed` message's `state` field, and it is
/// the single source of truth the UI renders from -- this app does not keep
/// separate optimistic local state.
///
/// The active position is a (rig index, preset index) pair rather than a bare
/// preset id, because presets live inside rigs and two rigs may both have a
/// preset called "Solo". The daemon additionally derives `active_rig_id` and
/// `active_preset_id` into the state view for convenience; those are exposed
/// here as [activeRigId]/[activePresetId] but the indices remain
/// authoritative.
class DaemonState {
  final int version;
  final List<Rig> rigs;
  final Map<String, Asset> assets;

  /// Wire-format keys are strings (JSON object keys always are, and that's
  /// how the daemon's pydantic model serializes its `Dict[int, ...]`), but
  /// they're switch indices, so this exposes `int` keys for convenient use
  /// in Dart.
  final Map<int, FootswitchAction> footswitchMapping;

  final int activeRigIndex;
  final int activePresetIndex;
  final String? activeRigId;
  final String? activePresetId;
  final bool bypass;
  final double? tempoBpm;

  const DaemonState({
    this.version = 2,
    this.rigs = const [],
    this.assets = const {},
    this.footswitchMapping = const {},
    this.activeRigIndex = 0,
    this.activePresetIndex = 0,
    this.activeRigId,
    this.activePresetId,
    this.bypass = false,
    this.tempoBpm,
  });

  /// A blank state to render before the first `state_snapshot` arrives.
  static const empty = DaemonState();

  Rig? get activeRig =>
      (activeRigIndex >= 0 && activeRigIndex < rigs.length)
          ? rigs[activeRigIndex]
          : null;

  Preset? get activePreset {
    final rig = activeRig;
    if (rig == null) return null;
    if (activePresetIndex < 0 || activePresetIndex >= rig.presets.length) {
      return null;
    }
    return rig.presets[activePresetIndex];
  }

  Rig? rigById(String id) {
    for (final rig in rigs) {
      if (rig.id == id) return rig;
    }
    return null;
  }

  factory DaemonState.fromJson(Map<String, dynamic> json) {
    final assetsJson = json['assets'] as Map<String, dynamic>? ?? const {};
    final mappingJson =
        json['footswitch_mapping'] as Map<String, dynamic>? ?? const {};

    return DaemonState(
      version: (json['version'] as num?)?.toInt() ?? 2,
      rigs: (json['rigs'] as List<dynamic>? ?? const [])
          .map((r) => Rig.fromJson(r as Map<String, dynamic>))
          .toList(),
      assets: assetsJson.map(
        (id, v) => MapEntry(id, Asset.fromJson(v as Map<String, dynamic>)),
      ),
      footswitchMapping: mappingJson.map(
        (k, v) => MapEntry(
          int.parse(k),
          FootswitchAction.fromJson(v as Map<String, dynamic>),
        ),
      ),
      activeRigIndex: (json['active_rig_index'] as num?)?.toInt() ?? 0,
      activePresetIndex: (json['active_preset_index'] as num?)?.toInt() ?? 0,
      activeRigId: json['active_rig_id'] as String?,
      activePresetId: json['active_preset_id'] as String?,
      bypass: json['bypass'] as bool? ?? false,
      tempoBpm: (json['tempo_bpm'] as num?)?.toDouble(),
    );
  }

  /// Note: `active_rig_id`/`active_preset_id` are derived by the daemon and
  /// are not part of its stored schema, so they are deliberately omitted
  /// here -- this round-trips the stored shape, not the state view.
  Map<String, dynamic> toJson() => {
        'version': version,
        'rigs': rigs.map((r) => r.toJson()).toList(),
        'assets': assets.map((id, a) => MapEntry(id, a.toJson())),
        'footswitch_mapping': footswitchMapping
            .map((k, v) => MapEntry(k.toString(), v.toJson())),
        'active_rig_index': activeRigIndex,
        'active_preset_index': activePresetIndex,
        'bypass': bypass,
        'tempo_bpm': tempoBpm,
      };
}
