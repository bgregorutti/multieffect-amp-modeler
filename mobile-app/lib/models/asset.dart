import 'block_param_descriptor.dart';

/// Registry metadata for one uploaded `.nam`/IR file or installed `.vst3`
/// plugin bundle.
///
/// Mirrors `control_daemon.models.Asset`. The binary bytes themselves never
/// pass through this model or through daemon state -- see
/// `asset_upload_service.dart` for the two-step HTTP-upload-then-WS-register
/// flow this metadata is part of (a `vst3` bundle is a directory, installed
/// out of band instead -- see `list_available_plugins`, planned).
enum AssetKind {
  nam,
  ir,
  vst3;

  static AssetKind fromWire(String value) {
    switch (value) {
      case 'nam':
        return AssetKind.nam;
      case 'ir':
        return AssetKind.ir;
      case 'vst3':
        return AssetKind.vst3;
      default:
        throw ArgumentError('Unknown asset kind: $value');
    }
  }

  String toWire() => name;
}

class Asset {
  final String id;
  final AssetKind kind;
  final String filename;
  final String storedPath;
  final int sizeBytes;
  final String? sha256;
  final double uploadedAt;

  /// The plugin's own parameter schema -- only ever non-null for
  /// `kind == AssetKind.vst3` (the engine introspects it as part of
  /// `register_asset`; see `BlockParamDescriptor`). A `nam`/`ir` asset's
  /// "schema" is nothing new, it's just whatever block type it's attached
  /// to (see `DaemonState.blockTypes` instead).
  final List<BlockParamDescriptor>? parameters;

  const Asset({
    required this.id,
    required this.kind,
    required this.filename,
    required this.storedPath,
    this.sizeBytes = 0,
    this.sha256,
    required this.uploadedAt,
    this.parameters,
  });

  factory Asset.fromJson(Map<String, dynamic> json) {
    final rawParameters = json['parameters'] as List<dynamic>?;
    return Asset(
      id: json['id'] as String,
      kind: AssetKind.fromWire(json['kind'] as String),
      filename: json['filename'] as String,
      storedPath: json['stored_path'] as String,
      sizeBytes: (json['size_bytes'] as num?)?.toInt() ?? 0,
      sha256: json['sha256'] as String?,
      uploadedAt: (json['uploaded_at'] as num).toDouble(),
      parameters: rawParameters
          ?.map((p) => BlockParamDescriptor.fromJson(p as Map<String, dynamic>))
          .toList(),
    );
  }

  Map<String, dynamic> toJson() => {
        'id': id,
        'kind': kind.toWire(),
        'filename': filename,
        'stored_path': storedPath,
        'size_bytes': sizeBytes,
        'sha256': sha256,
        'uploaded_at': uploadedAt,
        if (parameters != null)
          'parameters': parameters!.map((p) => p.toJson()).toList(),
      };
}
