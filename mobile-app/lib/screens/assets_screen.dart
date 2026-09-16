import 'package:flutter/material.dart';

import '../models/asset.dart';
import '../models/asset_category.dart';
import '../models/ws_messages.dart';
import '../services/asset_upload_service.dart';
import '../state/daemon_state_controller.dart';

/// Callback that produces a file to upload, for a given asset `kind` ("nam"
/// or "ir"). In a real build this is backed by an OS file picker (e.g. the
/// `file_picker` package), which needs platform channels this headless
/// sandbox cannot exercise -- see the mobile-app README's "what's stubbed"
/// section. Injected so the screen and the upload flow are fully testable
/// without one.
typedef FilePickerCallback = Future<PickedFile?> Function(String kind);

/// Lists uploaded assets (kind/filename/size) and lets the user trigger an
/// upload for a chosen kind.
class AssetsScreen extends StatefulWidget {
  final DaemonStateController controller;
  final AssetUploadService uploadService;
  final FilePickerCallback pickFile;

  const AssetsScreen({
    super.key,
    required this.controller,
    required this.uploadService,
    required this.pickFile,
  });

  @override
  State<AssetsScreen> createState() => _AssetsScreenState();
}

class _AssetsScreenState extends State<AssetsScreen> {
  bool _uploading = false;
  String? _lastError;

  Future<void> _upload(String kind) async {
    final file = await widget.pickFile(kind);
    if (file == null) return;

    setState(() {
      _uploading = true;
      _lastError = null;
    });
    try {
      await widget.uploadService.uploadAndRegister(
        kind: kind,
        file: file,
        daemonClient: widget.controller.client,
      );
    } catch (e) {
      setState(() => _lastError = e.toString());
    } finally {
      if (mounted) setState(() => _uploading = false);
    }
  }

  String _formatSize(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KB';
    return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';
  }

  Future<void> _rename(Asset asset) async {
    final newName = await showDialog<String>(
      context: context,
      builder: (context) => _RenameDialog(initialValue: asset.displayLabel),
    );
    if (newName == null || !mounted) return;
    final trimmed = newName.trim();
    // The daemon's rename_asset requires a non-empty display_name -- an
    // emptied field resets to the filename, the same default
    // register_asset itself uses, rather than sending an unsupported null.
    await widget.controller.client.renameAsset(
      RenameAssetCommand(
        assetId: asset.id,
        displayName: trimmed.isEmpty ? asset.filename : trimmed,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Assets'),
        actions: [
          IconButton(
            key: const Key('upload-nam-button'),
            icon: const Icon(Icons.upload_file),
            tooltip: 'Upload NAM model',
            onPressed: _uploading ? null : () => _upload('nam'),
          ),
          IconButton(
            key: const Key('upload-ir-button'),
            icon: const Icon(Icons.graphic_eq),
            tooltip: 'Upload IR file',
            onPressed: _uploading ? null : () => _upload('ir'),
          ),
        ],
      ),
      body: Column(
        children: [
          if (_uploading)
            const LinearProgressIndicator(key: Key('upload-progress')),
          if (_lastError != null)
            Padding(
              padding: const EdgeInsets.all(8.0),
              child: Text(
                _lastError!,
                key: const Key('upload-error-text'),
                style: const TextStyle(color: Colors.red),
              ),
            ),
          Expanded(
            child: ListenableBuilder(
              listenable: widget.controller,
              builder: (context, _) {
                final assets = widget.controller.state.assets.values.toList()
                  ..sort((a, b) => a.filename.compareTo(b.filename));

                if (assets.isEmpty) {
                  return const Center(child: Text('No assets uploaded yet'));
                }

                return ListView.builder(
                  padding: const EdgeInsets.symmetric(vertical: 4),
                  itemCount: assets.length,
                  itemBuilder: (context, index) {
                    final Asset asset = assets[index];
                    return Card(
                      key: Key('asset-tile-${asset.id}'),
                      margin:
                          const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
                      elevation: 0,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(16),
                        side: BorderSide(
                          color: Theme.of(context)
                              .colorScheme
                              .outlineVariant
                              .withValues(alpha: 0.4),
                        ),
                      ),
                      child: ListTile(
                        leading: Icon(assetKindIcon(asset.kind)),
                        title: Text(asset.displayLabel),
                        subtitle: Text(
                          '${asset.kind.toWire().toUpperCase()} • ${_formatSize(asset.sizeBytes)}',
                        ),
                        trailing: IconButton(
                          key: Key('rename-asset-button-${asset.id}'),
                          icon: const Icon(Icons.edit_outlined),
                          tooltip: 'Rename',
                          onPressed: () => _rename(asset),
                        ),
                      ),
                    );
                  },
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}

/// The rename dialog's content, as its own `StatefulWidget` so its
/// `TextEditingController` is disposed by Flutter's normal widget lifecycle
/// (when this element is actually removed from the tree, after the dialog's
/// closing animation finishes) rather than by the caller right after
/// `await showDialog(...)` returns -- that future completes as soon as
/// `Navigator.pop` is called, while the dialog's exit transition is still
/// animating and still using the controller, so disposing it there hits a
/// "TextEditingController used after being disposed" race.
class _RenameDialog extends StatefulWidget {
  final String initialValue;

  const _RenameDialog({required this.initialValue});

  @override
  State<_RenameDialog> createState() => _RenameDialogState();
}

class _RenameDialogState extends State<_RenameDialog> {
  late final TextEditingController _controller =
      TextEditingController(text: widget.initialValue);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('Rename'),
      content: TextField(
        key: const Key('rename-asset-input'),
        controller: _controller,
        autofocus: true,
        decoration: const InputDecoration(labelText: 'Name'),
        onSubmitted: (v) => Navigator.of(context).pop(v),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('Cancel'),
        ),
        TextButton(
          key: const Key('confirm-rename-asset'),
          onPressed: () => Navigator.of(context).pop(_controller.text),
          child: const Text('Save'),
        ),
      ],
    );
  }
}
