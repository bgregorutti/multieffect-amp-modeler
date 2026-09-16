import 'package:flutter/material.dart';

/// One slot in a horizontal, DAW-style signal-chain strip
/// (`[Gain] -> [Head] -> [Cab] -> [Effects] -> [EQ] -> [Volume]`), used by
/// both `RigChainEditorScreen` (the rig's backline) and
/// `PresetEditorScreen` (a preset's pinned + switchable blocks) so the two
/// screens share one visual language for "a block in the chain".
///
/// Three visual states, chosen by the caller (never inferred here, since
/// what counts as "empty" differs -- a backline slot is empty when it wants
/// an asset it doesn't have; a native gain/eq block is never empty):
/// - **filled** (default): icon + label, normal Material 3 card, tappable.
/// - **empty** (`isEmpty: true`): muted/dashed-looking placeholder with a
///   "+" glyph, prompting a tap to assign something.
/// - **locked** (`isLocked: true`): muted, no tap target at all -- the
///   rig-inherited pinned blocks on `PresetEditorScreen`, which a preset is
///   never allowed to switch or edit (see the small lock glyph).
class ChainStageCard extends StatelessWidget {
  final IconData icon;
  final String label;
  final String? subtitle;
  final bool isEmpty;
  final bool isLocked;
  final VoidCallback? onTap;

  /// Small overlay in the top-right corner, e.g. a remove ("x") button.
  /// Never shown on a locked card -- there is nothing to do to a pinned
  /// block from here.
  final Widget? corner;

  /// Extra content below the label, e.g. a compact enable switch.
  final Widget? footer;

  final double width;

  const ChainStageCard({
    super.key,
    required this.icon,
    required this.label,
    this.subtitle,
    this.isEmpty = false,
    this.isLocked = false,
    this.onTap,
    this.corner,
    this.footer,
    this.width = 120,
  });

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final muted = isEmpty || isLocked;

    final card = Container(
      width: width,
      constraints: const BoxConstraints(minHeight: 96),
      decoration: BoxDecoration(
        color: isLocked
            ? scheme.surfaceContainerHighest.withValues(alpha: 0.5)
            : isEmpty
                ? scheme.surfaceContainerHighest.withValues(alpha: 0.35)
                : scheme.surfaceContainerHigh,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(
          color: isEmpty
              ? scheme.outlineVariant
              : isLocked
                  ? Colors.transparent
                  : scheme.outlineVariant.withValues(alpha: 0.4),
          width: isEmpty ? 1.5 : 1,
          style: BorderStyle.solid,
        ),
      ),
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 12),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            isEmpty ? Icons.add_circle_outline : icon,
            size: 28,
            color: muted
                ? scheme.onSurfaceVariant.withValues(alpha: 0.7)
                : scheme.primary,
          ),
          const SizedBox(height: 6),
          Text(
            label,
            textAlign: TextAlign.center,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: Theme.of(context).textTheme.labelMedium?.copyWith(
                  color: muted ? scheme.onSurfaceVariant : scheme.onSurface,
                  fontWeight: FontWeight.w600,
                ),
          ),
          if (subtitle != null)
            Padding(
              padding: const EdgeInsets.only(top: 2),
              child: Text(
                subtitle!,
                textAlign: TextAlign.center,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: Theme.of(context).textTheme.labelSmall?.copyWith(
                      color: scheme.onSurfaceVariant,
                    ),
              ),
            ),
          if (isLocked)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Icon(Icons.lock_outline,
                  size: 14, color: scheme.onSurfaceVariant),
            ),
          if (footer != null)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              // A ListTile-family widget (e.g. a compact SwitchListTile)
              // paints its own ink/background on the nearest Material
              // ancestor -- without its own Material here, that would be
              // this card's colored Container above, which Flutter flags
              // as hiding the effect.
              child: Material(type: MaterialType.transparency, child: footer),
            ),
        ],
      ),
    );

    return Stack(
      clipBehavior: Clip.none,
      children: [
        Material(
          color: Colors.transparent,
          borderRadius: BorderRadius.circular(16),
          child: InkWell(
            borderRadius: BorderRadius.circular(16),
            onTap: onTap,
            child: card,
          ),
        ),
        if (corner != null && !isLocked)
          Positioned(top: -6, right: -6, child: corner!),
      ],
    );
  }
}
