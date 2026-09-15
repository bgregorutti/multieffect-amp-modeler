/// One adjustable parameter's schema -- what the live-controls UI needs to
/// render a real control (a `Slider` bound to [min]/[max]/[unit], or a
/// discrete picker when [stepCount] > 0) instead of the generic key/value
/// text editor `RigChainEditorScreen` uses for a block's *defaults*.
///
/// Mirrors `control_daemon.models.BlockParamDescriptor`. Metadata only,
/// never a value -- see `PresetBlockState.params` for where a live-tweaked
/// value actually lives. [key] is a native block's own param name (e.g.
/// `"gain_db"`, see `DaemonState.blockTypes`) or a `vst3` plugin's own
/// stringified VST3 `ParamID` (see `Asset.parameters`).
class BlockParamDescriptor {
  final String key;
  final String label;
  final String unit;
  final double min;
  final double max;
  final double defaultValue;
  final int stepCount;

  const BlockParamDescriptor({
    required this.key,
    required this.label,
    this.unit = '',
    this.min = 0.0,
    this.max = 1.0,
    this.defaultValue = 0.0,
    this.stepCount = 0,
  });

  factory BlockParamDescriptor.fromJson(Map<String, dynamic> json) {
    return BlockParamDescriptor(
      key: json['key'] as String,
      label: json['label'] as String,
      unit: json['unit'] as String? ?? '',
      min: (json['min'] as num?)?.toDouble() ?? 0.0,
      max: (json['max'] as num?)?.toDouble() ?? 1.0,
      defaultValue: (json['default'] as num?)?.toDouble() ?? 0.0,
      stepCount: (json['step_count'] as num?)?.toInt() ?? 0,
    );
  }

  Map<String, dynamic> toJson() => {
        'key': key,
        'label': label,
        'unit': unit,
        'min': min,
        'max': max,
        'default': defaultValue,
        'step_count': stepCount,
      };
}

/// A native block type's static parameter schema (from the `list_block_types`
/// command) -- static per engine build, not per-rig state.
class BlockTypeDescriptor {
  final String type;
  final List<BlockParamDescriptor> parameters;

  const BlockTypeDescriptor({required this.type, this.parameters = const []});

  factory BlockTypeDescriptor.fromJson(Map<String, dynamic> json) {
    return BlockTypeDescriptor(
      type: json['type'] as String,
      parameters: (json['parameters'] as List<dynamic>? ?? const [])
          .map((p) => BlockParamDescriptor.fromJson(p as Map<String, dynamic>))
          .toList(),
    );
  }

  Map<String, dynamic> toJson() => {
        'type': type,
        'parameters': parameters.map((p) => p.toJson()).toList(),
      };
}
