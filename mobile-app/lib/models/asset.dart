/// Registry metadata for one uploaded `.nam` model or IR file.
///
/// Mirrors `control_daemon.models.Asset`. The binary bytes themselves never
/// pass through this model or through daemon state -- see
/// `asset_upload_service.dart` for the two-step HTTP-upload-then-WS-register
/// flow this metadata is part of.
enum AssetKind {
  nam,
  ir;

  static AssetKind fromWire(String value) {
    switch (value) {
      case 'nam':
        return AssetKind.nam;
      case 'ir':
        return AssetKind.ir;
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

  const Asset({
    required this.id,
    required this.kind,
    required this.filename,
    required this.storedPath,
    this.sizeBytes = 0,
    this.sha256,
    required this.uploadedAt,
  });

  factory Asset.fromJson(Map<String, dynamic> json) {
    return Asset(
      id: json['id'] as String,
      kind: AssetKind.fromWire(json['kind'] as String),
      filename: json['filename'] as String,
      storedPath: json['stored_path'] as String,
      sizeBytes: (json['size_bytes'] as num?)?.toInt() ?? 0,
      sha256: json['sha256'] as String?,
      uploadedAt: (json['uploaded_at'] as num).toDouble(),
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
      };
}
