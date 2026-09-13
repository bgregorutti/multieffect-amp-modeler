import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/asset.dart';

import 'protocol_fixtures.dart';

void main() {
  group('Asset', () {
    test('parses an asset from the full daemon state fixture', () {
      final assetJson =
          fullDaemonStateFixture['assets'] as Map<String, dynamic>;
      final asset = Asset.fromJson(
        assetJson['nam1'] as Map<String, dynamic>,
      );

      expect(asset.id, 'nam1');
      expect(asset.kind, AssetKind.nam);
      expect(asset.filename, 'my_amp.nam');
      expect(asset.storedPath, '/data/assets/abc123.nam');
      expect(asset.sizeBytes, 20971520);
      expect(asset.sha256, isNotNull);
      expect(asset.sha256!.length, 64);
      expect(asset.uploadedAt, 999.0);
    });

    test('round-trips through toJson/fromJson', () {
      const original = Asset(
        id: 'irX',
        kind: AssetKind.ir,
        filename: 'cab.wav',
        storedPath: '/data/assets/x.wav',
        sizeBytes: 512,
        sha256: null,
        uploadedAt: 42.0,
      );

      final roundTripped = Asset.fromJson(original.toJson());

      expect(roundTripped.id, original.id);
      expect(roundTripped.kind, original.kind);
      expect(roundTripped.filename, original.filename);
      expect(roundTripped.storedPath, original.storedPath);
      expect(roundTripped.sizeBytes, original.sizeBytes);
      expect(roundTripped.sha256, original.sha256);
      expect(roundTripped.uploadedAt, original.uploadedAt);
    });

    test('AssetKind wire round-trip', () {
      expect(AssetKind.fromWire('nam'), AssetKind.nam);
      expect(AssetKind.fromWire('ir'), AssetKind.ir);
      expect(AssetKind.nam.toWire(), 'nam');
      expect(AssetKind.ir.toWire(), 'ir');
      expect(() => AssetKind.fromWire('bogus'), throwsArgumentError);
    });
  });
}
