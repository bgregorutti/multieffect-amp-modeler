import 'package:flutter/material.dart';

/// The `->` between two `ChainStageCard`s in a horizontal signal-chain
/// strip. Purely decorative -- never tappable -- so the DAW-style
/// `[Gain] -> [Head] -> [Cab] -> ...` reads as one continuous signal path.
class ChainConnector extends StatelessWidget {
  const ChainConnector({super.key});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 2),
      child: Icon(
        Icons.arrow_forward_rounded,
        size: 20,
        color: Theme.of(context).colorScheme.outline,
      ),
    );
  }
}
