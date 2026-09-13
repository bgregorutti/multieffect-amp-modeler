/// What a physical footswitch press does, once mapped.
///
/// Mirrors the `FootswitchAction` discriminated union in
/// `control_daemon.models` (a `Union` of five pydantic models, discriminated
/// on their `type` literal). Modeled here as a sealed class hierarchy so
/// Dart's exhaustiveness checking (switch over a sealed type) catches a
/// missing case at compile time if a new action type is ever added.
sealed class FootswitchAction {
  const FootswitchAction();

  String get wireType;

  factory FootswitchAction.fromJson(Map<String, dynamic> json) {
    final type = json['type'] as String;
    switch (type) {
      case 'select_slot':
        return SelectSlotAction(slot: (json['slot'] as num).toInt());
      case 'next_bank':
        return const NextBankAction();
      case 'prev_bank':
        return const PrevBankAction();
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

class SelectSlotAction extends FootswitchAction {
  final int slot;

  const SelectSlotAction({required this.slot});

  @override
  String get wireType => 'select_slot';

  @override
  Map<String, dynamic> toJson() => {'type': wireType, 'slot': slot};

  @override
  bool operator ==(Object other) =>
      other is SelectSlotAction && other.slot == slot;

  @override
  int get hashCode => Object.hash(wireType, slot);
}

class NextBankAction extends FootswitchAction {
  const NextBankAction();

  @override
  String get wireType => 'next_bank';

  @override
  Map<String, dynamic> toJson() => {'type': wireType};

  @override
  bool operator ==(Object other) => other is NextBankAction;

  @override
  int get hashCode => wireType.hashCode;
}

class PrevBankAction extends FootswitchAction {
  const PrevBankAction();

  @override
  String get wireType => 'prev_bank';

  @override
  Map<String, dynamic> toJson() => {'type': wireType};

  @override
  bool operator ==(Object other) => other is PrevBankAction;

  @override
  int get hashCode => wireType.hashCode;
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
const List<String> kFootswitchActionKinds = [
  'select_slot',
  'next_bank',
  'prev_bank',
  'toggle_bypass',
  'tap_tempo',
];
