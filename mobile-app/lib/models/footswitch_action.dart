/// What a physical footswitch press does, once mapped.
///
/// Mirrors the `FootswitchAction` discriminated union in
/// `control_daemon.models` (a `Union` of pydantic models, discriminated on
/// their `type` literal). Modeled here as a sealed class hierarchy so Dart's
/// exhaustiveness checking (switch over a sealed type) catches a missing case
/// at compile time if a new action type is ever added.
///
/// The two stepping pairs are not interchangeable. Rig stepping swaps the
/// whole backline, so the engine reloads its NAM model and IR; preset
/// stepping only flips which effects are on under an unchanged amp and cab,
/// which is the fast, click-free path. Preset stepping deliberately stays
/// inside the current rig and never crosses into another one.
sealed class FootswitchAction {
  const FootswitchAction();

  String get wireType;

  factory FootswitchAction.fromJson(Map<String, dynamic> json) {
    final type = json['type'] as String;
    switch (type) {
      case 'next_rig':
        return const NextRigAction();
      case 'prev_rig':
        return const PrevRigAction();
      case 'next_preset':
        return const NextPresetAction();
      case 'prev_preset':
        return const PrevPresetAction();
      case 'select_preset':
        return SelectPresetAction(index: (json['index'] as num).toInt());
      case 'toggle_bypass':
        return const ToggleBypassAction();
      case 'tap_tempo':
        return const TapTempoAction();
      default:
        throw ArgumentError('Unknown footswitch action type: $type');
    }
  }

  Map<String, dynamic> toJson();
}

class NextRigAction extends FootswitchAction {
  const NextRigAction();

  @override
  String get wireType => 'next_rig';

  @override
  Map<String, dynamic> toJson() => {'type': wireType};

  @override
  bool operator ==(Object other) => other is NextRigAction;

  @override
  int get hashCode => wireType.hashCode;
}

class PrevRigAction extends FootswitchAction {
  const PrevRigAction();

  @override
  String get wireType => 'prev_rig';

  @override
  Map<String, dynamic> toJson() => {'type': wireType};

  @override
  bool operator ==(Object other) => other is PrevRigAction;

  @override
  int get hashCode => wireType.hashCode;
}

class NextPresetAction extends FootswitchAction {
  const NextPresetAction();

  @override
  String get wireType => 'next_preset';

  @override
  Map<String, dynamic> toJson() => {'type': wireType};

  @override
  bool operator ==(Object other) => other is NextPresetAction;

  @override
  int get hashCode => wireType.hashCode;
}

class PrevPresetAction extends FootswitchAction {
  const PrevPresetAction();

  @override
  String get wireType => 'prev_preset';

  @override
  Map<String, dynamic> toJson() => {'type': wireType};

  @override
  bool operator ==(Object other) => other is PrevPresetAction;

  @override
  int get hashCode => wireType.hashCode;
}

/// Jump straight to the Nth preset of the current rig -- for boards with
/// enough switches to address presets directly rather than stepping.
class SelectPresetAction extends FootswitchAction {
  final int index;

  const SelectPresetAction({required this.index});

  @override
  String get wireType => 'select_preset';

  @override
  Map<String, dynamic> toJson() => {'type': wireType, 'index': index};

  @override
  bool operator ==(Object other) =>
      other is SelectPresetAction && other.index == index;

  @override
  int get hashCode => Object.hash(wireType, index);
}

class ToggleBypassAction extends FootswitchAction {
  const ToggleBypassAction();

  @override
  String get wireType => 'toggle_bypass';

  @override
  Map<String, dynamic> toJson() => {'type': wireType};

  @override
  bool operator ==(Object other) => other is ToggleBypassAction;

  @override
  int get hashCode => wireType.hashCode;
}

class TapTempoAction extends FootswitchAction {
  const TapTempoAction();

  @override
  String get wireType => 'tap_tempo';

  @override
  Map<String, dynamic> toJson() => {'type': wireType};

  @override
  bool operator ==(Object other) => other is TapTempoAction;

  @override
  int get hashCode => wireType.hashCode;
}

/// All action kinds a footswitch mapping UI can offer, in a stable order.
/// Rig/preset stepping comes first: that is the four-switch layout the pedal
/// is built around (up/down = rig, left/right = preset).
const List<String> kFootswitchActionKinds = [
  'next_rig',
  'prev_rig',
  'next_preset',
  'prev_preset',
  'select_preset',
  'toggle_bypass',
  'tap_tempo',
];
