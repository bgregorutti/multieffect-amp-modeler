import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:mobile_app/screens/assets_screen.dart';
import 'package:mobile_app/services/asset_upload_service.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  testWidgets('lists uploaded assets with kind and size', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);
    final uploadService = AssetUploadService(
      daemonHttpBaseUrl: Uri.parse('http://127.0.0.1:0'),
    );

    await tester.pumpWidget(MaterialApp(
      home: AssetsScreen(
        controller: controller,
        uploadService: uploadService,
        pickFile: (kind) async => null,
      ),
    ));

    expect(find.text('my_amp.nam'), findsOneWidget);
    expect(find.text('cab.wav'), findsOneWidget);
    expect(find.textContaining('NAM'), findsOneWidget);
    expect(find.textContaining('IR'), findsOneWidget);
  });

  testWidgets('shows an empty message when there are no assets',
      (tester) async {
    final fakeClient = FakeDaemonClient();
    final controller = DaemonStateController(fakeClient);
    final uploadService = AssetUploadService(
      daemonHttpBaseUrl: Uri.parse('http://127.0.0.1:0'),
    );

    await tester.pumpWidget(MaterialApp(
      home: AssetsScreen(
        controller: controller,
        uploadService: uploadService,
        pickFile: (kind) async => null,
      ),
    ));

    expect(find.text('No assets uploaded yet'), findsOneWidget);
  });

  testWidgets(
    'tapping upload invokes the injected file picker, then the upload '
    'service, then registers the asset over the daemon client',
    (tester) async {
      final fakeClient = FakeDaemonClient(state: sampleState);
      final controller = DaemonStateController(fakeClient);

      // A fake HTTP transport (no real socket) standing in for the daemon's
      // upload endpoint, so this screen test stays a pure widget test --
      // the real HTTP behavior of AssetUploadService itself is covered by
      // asset_upload_service_test.dart against a genuine local HttpServer.
      final mockHttpClient = MockClient((request) async {
        return http.Response(
          jsonEncode({
            'kind': request.url.queryParameters['kind'],
            'filename': request.url.queryParameters['filename'],
            'stored_path': '/data/assets/fake.bin',
            'size_bytes': request.bodyBytes.length,
            'sha256': 'deadbeef',
          }),
          200,
        );
      });
      final uploadService = AssetUploadService(
        daemonHttpBaseUrl: Uri.parse('http://127.0.0.1:0'),
        httpClient: mockHttpClient,
      );

      String? requestedKind;
      await tester.pumpWidget(MaterialApp(
        home: AssetsScreen(
          controller: controller,
          uploadService: uploadService,
          pickFile: (kind) async {
            requestedKind = kind;
            return PickedFile(
              filename: 'new_amp.nam',
              bytes: Uint8List.fromList([1, 2, 3]),
            );
          },
        ),
      ));

      await tester.tap(find.byKey(const Key('upload-nam-button')));
      await tester.pumpAndSettle();

      expect(requestedKind, 'nam');
      final registerCommands = fakeClient.sentCommands
          .where((c) => c.type == 'register_asset')
          .toList();
      expect(registerCommands, hasLength(1));
    },
  );

  testWidgets('a picker returning null does nothing', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);
    final uploadService = AssetUploadService(
      daemonHttpBaseUrl: Uri.parse('http://127.0.0.1:0'),
    );

    await tester.pumpWidget(MaterialApp(
      home: AssetsScreen(
        controller: controller,
        uploadService: uploadService,
        pickFile: (kind) async => null,
      ),
    ));

    await tester.tap(find.byKey(const Key('upload-ir-button')));
    await tester.pumpAndSettle();

    expect(
      fakeClient.sentCommands.where((c) => c.type == 'register_asset'),
      isEmpty,
    );
  });
}
