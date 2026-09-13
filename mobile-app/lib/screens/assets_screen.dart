import 'package:flutter/material.dart';

import '../models/asset.dart';
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
                  itemCount: assets.length,
                  itemBuilder: (context, index) {
                    final Asset asset = assets[index];
                    return ListTile(
                      key: Key('asset-tile-${asset.id}'),
                      leading: Icon(
                        asset.kind == AssetKind.nam
                            ? Icons.memory
                            : Icons.graphic_eq,
                      ),
                      title: Text(asset.filename),
                      subtitle: Text(
                        '${asset.kind.toWire().toUpperCase()} • ${_formatSize(asset.sizeBytes)}',
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
