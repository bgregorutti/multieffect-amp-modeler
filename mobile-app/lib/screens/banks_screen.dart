import 'package:flutter/material.dart';

import '../models/bank.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';

/// Lists/creates banks, assigns presets to slots, and reorders banks
/// (`reorder_banks`).
class BanksScreen extends StatelessWidget {
  final DaemonStateController controller;

  const BanksScreen({super.key, required this.controller});

  Future<void> _createBank(BuildContext context) async {
    final name = await showDialog<String>(
      context: context,
      builder: (context) {
        final textController = TextEditingController();
        return AlertDialog(
          title: const Text('New bank'),
          content: TextField(
            key: const Key('bank-name-field'),
            controller: textController,
            autofocus: true,
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(context).pop(),
              child: const Text('Cancel'),
            ),
            TextButton(
              key: const Key('bank-name-save'),
              onPressed: () =>
                  Navigator.of(context).pop(textController.text.trim()),
              child: const Text('Create'),
            ),
          ],
        );
      },
    );
    if (name == null || name.isEmpty) return;
    await controller.client.createBank(CreateBankCommand(name: name));
  }

  Future<void> _assignSlot(
    BuildContext context,
    Bank bank,
    int slotIndex,
  ) async {
    final presets = controller.state.presets.values.toList()
      ..sort((a, b) => a.name.compareTo(b.name));
    final selected = await showDialog<String?>(
      context: context,
      builder: (context) => SimpleDialog(
        title: Text('Slot ${slotIndex + 1}'),
        children: [
          SimpleDialogOption(
            key: const Key('slot-choice-none'),
            onPressed: () => Navigator.of(context).pop(),
            child: const Text('(empty)'),
          ),
          for (final preset in presets)
            SimpleDialogOption(
              key: Key('slot-choice-${preset.id}'),
              onPressed: () => Navigator.of(context).pop(preset.id),
              child: Text(preset.name),
            ),
        ],
      ),
    );
    final nextSlots = List<String?>.of(bank.slots);
    nextSlots[slotIndex] = selected;
    await controller.client.updateBank(
      UpdateBankCommand(bankId: bank.id, slots: nextSlots),
    );
  }

  Future<void> _moveBank(int index, int delta) async {
    final banks = controller.state.banks;
    final newIndex = index + delta;
    if (newIndex < 0 || newIndex >= banks.length) return;
    final ids = banks.map((b) => b.id).toList();
    final id = ids.removeAt(index);
    ids.insert(newIndex, id);
    await controller.client.reorderBanks(ReorderBanksCommand(bankIds: ids));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Banks'),
        actions: [
          IconButton(
            key: const Key('add-bank-button'),
            icon: const Icon(Icons.add),
            onPressed: () => _createBank(context),
          ),
        ],
      ),
      body: ListenableBuilder(
        listenable: controller,
        builder: (context, _) {
          final banks = controller.state.banks;
          final presets = controller.state.presets;

          if (banks.isEmpty) {
            return const Center(child: Text('No banks yet'));
          }

          return ListView.builder(
            itemCount: banks.length,
            itemBuilder: (context, bankIndex) {
              final bank = banks[bankIndex];
              return Card(
                key: Key('bank-card-${bank.id}'),
                margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                child: Padding(
                  padding: const EdgeInsets.all(8.0),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          Expanded(
                            child: Text(
                              bank.name,
                              style: Theme.of(context).textTheme.titleMedium,
                            ),
                          ),
                          IconButton(
                            key: Key('move-bank-up-${bank.id}'),
                            icon: const Icon(Icons.arrow_upward),
                            onPressed: () => _moveBank(bankIndex, -1),
                          ),
                          IconButton(
                            key: Key('move-bank-down-${bank.id}'),
                            icon: const Icon(Icons.arrow_downward),
                            onPressed: () => _moveBank(bankIndex, 1),
                          ),
                        ],
                      ),
                      Wrap(
                        spacing: 8,
                        runSpacing: 8,
                        children: [
                          for (var i = 0; i < bank.slots.length; i++)
                            ActionChip(
                              key: Key('bank-slot-${bank.id}-$i'),
                              label: Text(
                                bank.slots[i] != null
                                    ? (presets[bank.slots[i]]?.name ??
                                        bank.slots[i]!)
                                    : 'Slot ${i + 1}: empty',
                              ),
                              onPressed: () => _assignSlot(context, bank, i),
                            ),
                        ],
                      ),
                    ],
                  ),
                ),
              );
            },
          );
        },
      ),
    );
  }
}
