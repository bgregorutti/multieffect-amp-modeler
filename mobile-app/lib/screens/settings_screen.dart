import 'package:flutter/material.dart';

import '../services/daemon_endpoint_store.dart';

/// Lets the user point the app at the control daemon's actual address --
/// the pedal's Wi-Fi AP, its address on a shared home network, or a
/// USB-link address -- without rebuilding the app. Saving persists the
/// choice via [DaemonEndpointStore] and reconnects immediately.
class SettingsScreen extends StatefulWidget {
  final DaemonEndpoint currentEndpoint;
  final ValueChanged<DaemonEndpoint> onSave;

  const SettingsScreen({
    super.key,
    required this.currentEndpoint,
    required this.onSave,
  });

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  final _formKey = GlobalKey<FormState>();
  late final TextEditingController _hostController;
  late final TextEditingController _portController;

  @override
  void initState() {
    super.initState();
    _hostController =
        TextEditingController(text: widget.currentEndpoint.host);
    _portController =
        TextEditingController(text: widget.currentEndpoint.port.toString());
  }

  @override
  void dispose() {
    _hostController.dispose();
    _portController.dispose();
    super.dispose();
  }

  String? _validateHost(String? value) {
    if (value == null || value.trim().isEmpty) return 'Required';
    return null;
  }

  String? _validatePort(String? value) {
    final port = int.tryParse((value ?? '').trim());
    if (port == null || port <= 0 || port > 65535) {
      return 'Enter a port between 1 and 65535';
    }
    return null;
  }

  void _save() {
    if (!_formKey.currentState!.validate()) return;
    final endpoint = DaemonEndpoint(
      host: _hostController.text.trim(),
      port: int.parse(_portController.text.trim()),
    );
    widget.onSave(endpoint);
    Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Daemon Connection'),
        actions: [
          IconButton(
            key: const Key('save-settings-button'),
            icon: const Icon(Icons.check),
            onPressed: _save,
          ),
        ],
      ),
      body: Form(
        key: _formKey,
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            const Text(
              "Where the control daemon lives -- the pedal's Wi-Fi AP "
              'address, its LAN address on a shared network, or a USB-link '
              'address. Saving reconnects immediately.',
            ),
            const SizedBox(height: 16),
            TextFormField(
              key: const Key('host-field'),
              controller: _hostController,
              decoration: const InputDecoration(
                labelText: 'Host / IP',
                hintText: '192.168.1.42',
              ),
              validator: _validateHost,
            ),
            const SizedBox(height: 12),
            TextFormField(
              key: const Key('port-field'),
              controller: _portController,
              decoration: const InputDecoration(labelText: 'Port'),
              keyboardType: TextInputType.number,
              validator: _validatePort,
            ),
          ],
        ),
      ),
    );
  }
}
