import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/asset.dart';
import 'package:mobile_app/models/block_param_descriptor.dart';

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
      expect(AssetKind.fromWire('vst3'), AssetKind.vst3);
      expect(AssetKind.nam.toWire(), 'nam');
      expect(AssetKind.ir.toWire(), 'ir');
      expect(AssetKind.vst3.toWire(), 'vst3');
      expect(() => AssetKind.fromWire('bogus'), throwsArgumentError);
    });

    test('a vst3 asset round-trips its introspected parameter schema', () {
      const original = Asset(
        id: 'plug1',
        kind: AssetKind.vst3,
        filename: 'GainTest.vst3',
        storedPath: '/plugins/GainTest.vst3',
        uploadedAt: 42.0,
        parameters: [
          BlockParamDescriptor(
            key: '0',
            label: 'Gain',
            unit: 'x',
            min: 0.0,
            max: 2.0,
            defaultValue: 1.0,
          ),
        ],
      );

      final roundTripped = Asset.fromJson(original.toJson());

      expect(roundTripped.kind, AssetKind.vst3);
      expect(roundTripped.parameters, hasLength(1));
      expect(roundTripped.parameters!.single.key, '0');
      expect(roundTripped.parameters!.single.label, 'Gain');
      expect(roundTripped.parameters!.single.max, 2.0);
    });

    test('a nam/ir asset has no parameters field on the wire', () {
      const original = Asset(
        id: 'nam1',
        kind: AssetKind.nam,
        filename: 'amp.nam',
        storedPath: '/data/assets/amp.nam',
        uploadedAt: 1.0,
      );
      expect(original.toJson().containsKey('parameters'), isFalse);
      expect(Asset.fromJson(original.toJson()).parameters, isNull);
    });
  });
}
