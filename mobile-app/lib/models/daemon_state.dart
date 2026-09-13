import 'asset.dart';
import 'bank.dart';
import 'footswitch_action.dart';
import 'preset.dart';

/// The full persisted + in-memory state of the daemon.
///
/// Mirrors `control_daemon.models.DaemonState`. This is what arrives inside
/// every `state_snapshot`/`state_changed` message's `state` field, and it is
/// the single source of truth the UI renders from -- this app does not keep
/// separate optimistic local state.
class DaemonState {
  final int version;
  final Map<String, Preset> presets;
  final List<Bank> banks;
  final Map<String, Asset> assets;

  /// Wire-format keys are strings (JSON object keys always are, and that's
  /// how the daemon's pydantic model serializes its `Dict[int, ...]`), but
  /// they're switch indices, so this exposes `int` keys for convenient use
  /// in Dart.
  final Map<int, FootswitchAction> footswitchMapping;

  final int activeBankIndex;
  final int activeSlot;
  final String? activePresetId;
  final bool bypass;
  final double? tempoBpm;

  const DaemonState({
    this.version = 1,
    this.presets = const {},
    this.banks = const [],
    this.assets = const {},
    this.footswitchMapping = const {},
    this.activeBankIndex = 0,
    this.activeSlot = 0,
    this.activePresetId,
    this.bypass = false,
    this.tempoBpm,
  });

  /// A blank state to render before the first `state_snapshot` arrives.
  static const empty = DaemonState();

  Preset? get activePreset =>
      activePresetId == null ? null : presets[activePresetId];

  factory DaemonState.fromJson(Map<String, dynamic> json) {
    final presetsJson = json['presets'] as Map<String, dynamic>? ?? const {};
    final assetsJson = json['assets'] as Map<String, dynamic>? ?? const {};
    final mappingJson =
        json['footswitch_mapping'] as Map<String, dynamic>? ?? const {};

    return DaemonState(
      version: (json['version'] as num?)?.toInt() ?? 1,
      presets: presetsJson.map(
        (id, v) => MapEntry(id, Preset.fromJson(v as Map<String, dynamic>)),
      ),
      banks: (json['banks'] as List<dynamic>? ?? const [])
          .map((b) => Bank.fromJson(b as Map<String, dynamic>))
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
      activeBankIndex: (json['active_bank_index'] as num?)?.toInt() ?? 0,
      activeSlot: (json['active_slot'] as num?)?.toInt() ?? 0,
      activePresetId: json['active_preset_id'] as String?,
      bypass: json['bypass'] as bool? ?? false,
      tempoBpm: (json['tempo_bpm'] as num?)?.toDouble(),
    );
  }

  Map<String, dynamic> toJson() => {
        'version': version,
        'presets': presets.map((id, p) => MapEntry(id, p.toJson())),
        'banks': banks.map((b) => b.toJson()).toList(),
        'assets': assets.map((id, a) => MapEntry(id, a.toJson())),
        'footswitch_mapping': footswitchMapping
            .map((k, v) => MapEntry(k.toString(), v.toJson())),
        'active_bank_index': activeBankIndex,
        'active_slot': activeSlot,
        'active_preset_id': activePresetId,
        'bypass': bypass,
        'tempo_bpm': tempoBpm,
      };
}
