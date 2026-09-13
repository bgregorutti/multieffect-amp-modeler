import 'package:flutter/material.dart';

import '../models/footswitch_action.dart';
import '../models/ws_messages.dart';
import '../state/daemon_state_controller.dart';

/// Configures the footswitch mapping: which of the five action types
/// (`select_slot`, `next_bank`, `prev_bank`, `toggle_bypass`, `tap_tempo`)
/// each physical switch index triggers.
///
/// `set_footswitch_mapping` replaces the *entire* mapping (not a merge), so
/// this screen edits a local draft and sends the complete map on save.
class FootswitchMappingScreen extends StatefulWidget {
  final DaemonStateController controller;

  const FootswitchMappingScreen({super.key, required this.controller});

  @override
  State<FootswitchMappingScreen> createState() =>
      _FootswitchMappingScreenState();
}

class _FootswitchMappingScreenState extends State<FootswitchMappingScreen> {
  late Map<int, FootswitchAction> _mapping;

  @override
  void initState() {
    super.initState();
    _mapping = Map.of(widget.controller.state.footswitchMapping);
  }

  void _addSwitch() {
    setState(() {
      var nextIndex = 0;
      while (_mapping.containsKey(nextIndex)) {
        nextIndex++;
      }
      _mapping = {..._mapping, nextIndex: const ToggleBypassAction()};
    });
  }

  void _removeSwitch(int index) {
    setState(() {
      final next = Map.of(_mapping);
      next.remove(index);
      _mapping = next;
    });
  }

  void _setAction(int index, FootswitchAction action) {
    setState(() {
      final next = Map.of(_mapping);
      next[index] = action;
      _mapping = next;
    });
  }

  FootswitchAction _actionForKind(String kind, FootswitchAction current) {
    switch (kind) {
      case 'select_slot':
        return SelectSlotAction(
          slot: current is SelectSlotAction ? current.slot : 0,
        );
      case 'next_bank':
        return const NextBankAction();
      case 'prev_bank':
        return const PrevBankAction();
      case 'toggle_bypass':
        return const ToggleBypassAction();
      case 'tap_tempo':
        return const TapTempoAction();
      default:
        return current;
    }
  }

  Future<void> _save() async {
    await widget.controller.client.setFootswitchMapping(
      SetFootswitchMappingCommand(mapping: _mapping),
    );
  }

  @override
  Widget build(BuildContext context) {
    final indices = _mapping.keys.toList()..sort();

    return Scaffold(
      appBar: AppBar(
        title: const Text('Footswitch Mapping'),
        actions: [
          IconButton(
            key: const Key('add-switch-button'),
            icon: const Icon(Icons.add),
            onPressed: _addSwitch,
          ),
          IconButton(
            key: const Key('save-mapping-button'),
            icon: const Icon(Icons.check),
            onPressed: _save,
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          for (final index in indices)
            Card(
              key: Key('switch-row-$index'),
              margin: const EdgeInsets.symmetric(vertical: 4),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
                child: Row(
                  children: [
                    SizedBox(
                      width: 72,
                      child: Text('Switch $index'),
                    ),
                    Expanded(
                      child: DropdownButtonFormField<String>(
                        key: Key('action-kind-dropdown-$index'),
                        initialValue: _mapping[index]!.wireType,
                        items: [
                          for (final kind in kFootswitchActionKinds)
                            DropdownMenuItem(value: kind, child: Text(kind)),
                        ],
                        onChanged: (kind) {
                          if (kind == null) return;
                          _setAction(
                            index,
                            _actionForKind(kind, _mapping[index]!),
                          );
                        },
                      ),
                    ),
                    if (_mapping[index] is SelectSlotAction)
                      SizedBox(
                        width: 64,
                        child: TextFormField(
                          key: Key('select-slot-field-$index'),
                          initialValue:
                              (_mapping[index] as SelectSlotAction).slot.toString(),
                          keyboardType: TextInputType.number,
                          onChanged: (v) {
                            final slot = int.tryParse(v) ?? 0;
                            _setAction(index, SelectSlotAction(slot: slot));
                          },
                        ),
                      ),
                    IconButton(
                      key: Key('remove-switch-button-$index'),
                      icon: const Icon(Icons.delete_outline),
                      onPressed: () => _removeSwitch(index),
                    ),
                  ],
                ),
              ),
            ),
        ],
      ),
    );
  }
}
