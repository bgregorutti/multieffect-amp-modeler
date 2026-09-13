/// An ordered grouping of presets into footswitch-sized pages.
///
/// Mirrors `control_daemon.models.Bank`. `slots` is ordered and (as created
/// by the daemon) fixed-length; each entry is either a preset id or `null`
/// for an empty slot.
class Bank {
  final String id;
  final String name;
  final List<String?> slots;

  const Bank({
    required this.id,
    required this.name,
    this.slots = const [],
  });

  factory Bank.fromJson(Map<String, dynamic> json) {
    return Bank(
      id: json['id'] as String,
      name: json['name'] as String,
      slots: (json['slots'] as List<dynamic>? ?? const [])
          .map((s) => s as String?)
          .toList(),
    );
  }

  Map<String, dynamic> toJson() => {
        'id': id,
        'name': name,
        'slots': slots,
      };

  Bank copyWith({String? name, List<String?>? slots}) {
    return Bank(
      id: id,
      name: name ?? this.name,
      slots: slots ?? this.slots,
    );
  }
}
