import 'dart:convert';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import '../models/ws_messages.dart';
import 'daemon_client.dart';

/// Metadata returned by the daemon's `POST /assets/upload` HTTP endpoint,
/// before it has been registered into daemon state.
class UploadedAssetMetadata {
  final String kind;
  final String filename;
  final String storedPath;
  final int sizeBytes;
  final String sha256;

  const UploadedAssetMetadata({
    required this.kind,
    required this.filename,
    required this.storedPath,
    required this.sizeBytes,
    required this.sha256,
  });

  factory UploadedAssetMetadata.fromJson(Map<String, dynamic> json) {
    return UploadedAssetMetadata(
      kind: json['kind'] as String,
      filename: json['filename'] as String,
      storedPath: json['stored_path'] as String,
      sizeBytes: (json['size_bytes'] as num).toInt(),
      sha256: json['sha256'] as String,
    );
  }
}

/// A file to upload: raw bytes plus the metadata the upload endpoint wants
/// as query params. Kept deliberately dumb/serializable so a real OS file
/// picker (a platform-channel dependency this headless sandbox can't
/// exercise -- see the mobile-app README) can be swapped in later without
/// touching this service: whatever picks the file just needs to produce one
/// of these.
class PickedFile {
  final String filename;
  final Uint8List bytes;

  const PickedFile({required this.filename, required this.bytes});
}

/// Implements the daemon's deliberate two-step binary asset upload:
///
/// 1. `POST http://<host>:<port>/assets/upload?kind=..&filename=..` with the
///    raw file bytes as the request body (NOT multipart) -- see
///    control-daemon/README.md "HTTP: uploading a .nam/IR binary".
/// 2. Send `register_asset` over the existing WS connection with the
///    metadata that step returned, so the daemon actually adds it to
///    `DaemonState.assets` and broadcasts it. The HTTP step alone never
///    touches daemon state.
class AssetUploadService {
  final Uri daemonHttpBaseUrl;
  final http.Client _httpClient;

  AssetUploadService({
    required this.daemonHttpBaseUrl,
    http.Client? httpClient,
  }) : _httpClient = httpClient ?? http.Client();

  /// Step 1 only: streams [file]'s bytes to the daemon and returns the
  /// metadata it hands back. Does not touch daemon state.
  Future<UploadedAssetMetadata> uploadBytes({
    required String kind,
    required PickedFile file,
  }) async {
    final uploadUri = daemonHttpBaseUrl.replace(
      path: '/assets/upload',
      queryParameters: {'kind': kind, 'filename': file.filename},
    );

    final response = await _httpClient.post(
      uploadUri,
      headers: {'Content-Type': 'application/octet-stream'},
      body: file.bytes,
    );

    if (response.statusCode != 200) {
      throw AssetUploadException(
        'upload failed with status ${response.statusCode}: ${response.body}',
      );
    }

    return UploadedAssetMetadata.fromJson(
      jsonDecode(response.body) as Map<String, dynamic>,
    );
  }

  /// The full two-step flow: HTTP upload, then `register_asset` over the
  /// given [daemonClient]'s WS connection. Returns the `command_ok` result
  /// (which carries the registered `Asset`).
  Future<Map<String, dynamic>> uploadAndRegister({
    required String kind,
    required PickedFile file,
    required DaemonClientBase daemonClient,
  }) async {
    final metadata = await uploadBytes(kind: kind, file: file);
    return daemonClient.registerAsset(
      RegisterAssetCommand(
        kind: metadata.kind,
        filename: metadata.filename,
        storedPath: metadata.storedPath,
        sizeBytes: metadata.sizeBytes,
        sha256: metadata.sha256,
      ),
    );
  }

  void dispose() => _httpClient.close();
}

class AssetUploadException implements Exception {
  final String message;
  const AssetUploadException(this.message);

  @override
  String toString() => 'AssetUploadException: $message';
}
