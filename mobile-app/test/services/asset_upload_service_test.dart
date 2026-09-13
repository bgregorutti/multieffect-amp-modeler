/// Tests [AssetUploadService]'s HTTP upload step against a genuine local
/// `dart:io` `HttpServer` (not a mocked `http.Client`), asserting the raw
/// body bytes arrive intact and byte-for-byte, and that query params are set
/// correctly -- mirroring exactly the contract in
/// `control-daemon/README.md`'s "HTTP: uploading a .nam/IR binary" and the
/// real streaming endpoint in `control_daemon/app.py: upload_asset`.
library;

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/services/asset_upload_service.dart';
import 'package:mobile_app/services/daemon_client.dart';

/// A tiny real HTTP server standing in for the daemon's
/// `POST /assets/upload` endpoint: reads the raw streamed body (not
/// multipart), computes its sha256, and returns metadata shaped exactly
/// like the real endpoint's response.
class _FakeUploadServer {
  HttpServer? _server;
  final List<Map<String, String>> receivedQuery = [];
  final List<List<int>> receivedBodies = [];

  int get port => _server!.port;
  Uri get baseUrl => Uri.parse('http://127.0.0.1:$port');

  Future<void> start() async {
    _server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    _serve();
  }

  void _serve() async {
    await for (final request in _server!) {
      if (request.method == 'POST' && request.uri.path == '/assets/upload') {
        final bodyBytes = await _readFullBody(request);
        receivedQuery.add(request.uri.queryParameters);
        receivedBodies.add(bodyBytes);

        final kind = request.uri.queryParameters['kind'];
        final filename =
            request.uri.queryParameters['filename'] ?? 'asset.bin';

        if (kind != 'nam' && kind != 'ir') {
          request.response.statusCode = 400;
          request.response
              .write(jsonEncode({'error': "kind must be 'nam' or 'ir'"}));
          await request.response.close();
          continue;
        }

        final digest = sha256.convert(bodyBytes);

        request.response.headers.contentType = ContentType.json;
        request.response.write(jsonEncode({
          'kind': kind,
          'filename': filename,
          'stored_path': '/data/assets/fake-token-$filename',
          'size_bytes': bodyBytes.length,
          'sha256': digest.toString(),
        }));
        await request.response.close();
      } else {
        request.response.statusCode = 404;
        await request.response.close();
      }
    }
  }

  Future<List<int>> _readFullBody(HttpRequest request) async {
    final bytes = <int>[];
    await for (final chunk in request) {
      bytes.addAll(chunk);
    }
    return bytes;
  }

  Future<void> stop() async {
    await _server?.close(force: true);
  }
}

class _FakeDaemonClientForRegister extends DaemonClientBase {
  RegisterAssetCommand? lastRegisterCommand;

  @override
  ConnectionStatus get status => ConnectionStatus.connected;

  @override
  DaemonState get state => throw UnimplementedError();

  @override
  Stream<ConnectionStatus> get statusStream => const Stream.empty();

  @override
  Stream<DaemonState> get stateStream => const Stream.empty();

  @override
  Future<void> connect() async {}

  @override
  void dispose() {}

  @override
  Future<Map<String, dynamic>> registerAsset(RegisterAssetCommand cmd) async {
    lastRegisterCommand = cmd;
    return {
      'asset': {'id': 'registered-1', ...cmd.toJson()},
    };
  }

  @override
  Future<Map<String, dynamic>> createPreset(CreatePresetCommand cmd) =>
      throw UnimplementedError();
  @override
  Future<Map<String, dynamic>> updatePreset(UpdatePresetCommand cmd) =>
      throw UnimplementedError();
  @override
  Future<Map<String, dynamic>> deletePreset(DeletePresetCommand cmd) =>
      throw UnimplementedError();
  @override
  Future<Map<String, dynamic>> selectPreset(SelectPresetCommand cmd) =>
      throw UnimplementedError();
  @override
  Future<Map<String, dynamic>> createBank(CreateBankCommand cmd) =>
      throw UnimplementedError();
  @override
  Future<Map<String, dynamic>> updateBank(UpdateBankCommand cmd) =>
      throw UnimplementedError();
  @override
  Future<Map<String, dynamic>> reorderBanks(ReorderBanksCommand cmd) =>
      throw UnimplementedError();
  @override
  Future<Map<String, dynamic>> setBypass(SetBypassCommand cmd) =>
      throw UnimplementedError();
  @override
  Future<Map<String, dynamic>> setFootswitchMapping(
          SetFootswitchMappingCommand cmd) =>
      throw UnimplementedError();
}

void main() {
  late _FakeUploadServer server;
  late AssetUploadService service;

  setUp(() async {
    server = _FakeUploadServer();
    await server.start();
    service = AssetUploadService(daemonHttpBaseUrl: server.baseUrl);
  });

  tearDown(() async {
    service.dispose();
    await server.stop();
  });

  test('sends the raw bytes as the body (not multipart) with correct query params', () async {
    final bytes = Uint8List.fromList(List.generate(5000, (i) => i % 256));

    final metadata = await service.uploadBytes(
      kind: 'nam',
      file: PickedFile(filename: 'my_amp.nam', bytes: bytes),
    );

    expect(server.receivedQuery, hasLength(1));
    expect(server.receivedQuery.first['kind'], 'nam');
    expect(server.receivedQuery.first['filename'], 'my_amp.nam');

    // The raw bytes must arrive intact and byte-for-byte -- no multipart
    // envelope, no base64, no corruption.
    expect(server.receivedBodies.first, bytes);

    expect(metadata.kind, 'nam');
    expect(metadata.filename, 'my_amp.nam');
    expect(metadata.sizeBytes, bytes.length);
    expect(metadata.sha256, sha256.convert(bytes).toString());
  });

  test('an ir upload sets kind=ir in the query', () async {
    final bytes = Uint8List.fromList([1, 2, 3, 4]);
    final metadata = await service.uploadBytes(
      kind: 'ir',
      file: PickedFile(filename: 'cab.wav', bytes: bytes),
    );
    expect(server.receivedQuery.first['kind'], 'ir');
    expect(metadata.kind, 'ir');
  });

  test(
    'uploadAndRegister does the HTTP step then calls registerAsset over '
    'the WS client with the metadata the HTTP step returned',
    () async {
      final fakeClient = _FakeDaemonClientForRegister();
      final bytes = Uint8List.fromList([9, 9, 9]);

      final result = await service.uploadAndRegister(
        kind: 'nam',
        file: PickedFile(filename: 'amp.nam', bytes: bytes),
        daemonClient: fakeClient,
      );

      expect(fakeClient.lastRegisterCommand, isNotNull);
      expect(fakeClient.lastRegisterCommand!.kind, 'nam');
      expect(fakeClient.lastRegisterCommand!.filename, 'amp.nam');
      expect(fakeClient.lastRegisterCommand!.sizeBytes, bytes.length);
      expect(fakeClient.lastRegisterCommand!.sha256,
          sha256.convert(bytes).toString());
      expect(result['asset']['id'], 'registered-1');
    },
  );

  test('a non-200 response throws AssetUploadException', () async {
    await expectLater(
      service.uploadBytes(
        kind: 'not-a-real-kind',
        file: PickedFile(filename: 'x.nam', bytes: Uint8List(0)),
      ),
      throwsA(isA<AssetUploadException>()),
    );
  });

  test('a connection failure surfaces as an exception', () async {
    // Capture the URL before stopping the server -- nothing is listening on
    // it afterwards, so the POST below must fail rather than hang or throw
    // away the error.
    final baseUrl = server.baseUrl;
    await server.stop();
    final deadService = AssetUploadService(daemonHttpBaseUrl: baseUrl);
    await expectLater(
      deadService.uploadBytes(
        kind: 'nam',
        file: PickedFile(filename: 'x.nam', bytes: Uint8List(0)),
      ),
      throwsA(anything),
    );
    deadService.dispose();
  });
}
