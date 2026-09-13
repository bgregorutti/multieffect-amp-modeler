import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/bank.dart';

import 'protocol_fixtures.dart';

void main() {
  group('Bank', () {
    test('parses the update_bank README fixture slots shape', () {
      // update_bank's `slots` field is the same shape Bank.slots uses.
      final bank = Bank.fromJson({
        'id': updateBankFixture['bank_id'],
        'name': 'Live Set 1',
        'slots': updateBankFixture['slots'],
      });

      expect(bank.id, 'bank1');
      expect(bank.slots, ['abc123', null, null, null]);
    });

    test('parses the bank embedded in the full daemon state fixture', () {
      final banksJson = fullDaemonStateFixture['banks'] as List;
      final bank = Bank.fromJson(banksJson.first as Map<String, dynamic>);

      expect(bank.id, 'bank1');
      expect(bank.name, 'Live Set 1');
      expect(bank.slots, ['abc123', null, null, null]);
    });

    test('round-trips through toJson/fromJson', () {
      const original = Bank(id: 'b1', name: 'Bank 1', slots: [null, 'x', null]);
      final roundTripped = Bank.fromJson(original.toJson());

      expect(roundTripped.id, original.id);
      expect(roundTripped.name, original.name);
      expect(roundTripped.slots, original.slots);
    });
  });
}
