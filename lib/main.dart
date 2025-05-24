import 'package:flutter/material.dart';
import 'package:url_launcher/url_launcher.dart';

void main() => runApp(const WiiFinApp());

class WiiFinApp extends StatelessWidget {
  const WiiFinApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'WiiFin',
      home: const WiiFinHomePage(),
    );
  }
}

class WiiFinHomePage extends StatelessWidget {
  const WiiFinHomePage({super.key});

  static final ValueNotifier<Offset> cursorPosition = ValueNotifier(Offset.zero);

  @override
  Widget build(BuildContext context) {
    final screenWidth = MediaQuery.of(context).size.width;
    final isMobile = screenWidth < 600;
    final logoWidth = screenWidth * 0.65;
    return Scaffold(
      backgroundColor: const Color(0xFFF0F3F8),
      body: isMobile
          ? _buildContent(context, isMobile, logoWidth)
          : MouseRegion(
              cursor: SystemMouseCursors.none, // cache le curseur natif partout dans la zone
              opaque: false,
              child: Stack(
                children: [
                  Listener(
                    behavior: HitTestBehavior.translucent, // capte les events même sur zones transparentes
                    onPointerHover: (event) {
                      cursorPosition.value = event.position;
                    },
                    onPointerMove: (event) {
                      cursorPosition.value = event.position;
                    },
                    child: _buildContent(context, isMobile, logoWidth),
                  ),
                  const _CustomCursor(),
                ],
              ),
            ),
    );
  }

  Widget _buildContent(BuildContext context, bool isMobile, double logoWidth) {
    return Column(
      children: [
        // Partie supérieure : contenu central centré verticalement
        Expanded(
          child: Center(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.center, // toujours centré
              children: [
                SizedBox(
                  width: logoWidth.clamp(200, 1100),
                  child: Image.asset(
                    'assets/logo_wiifin_banner.png',
                    fit: BoxFit.contain,
                  ),
                ),
                const SizedBox(height: 40),
                // Texte toujours centré
                const SizedBox(
                  width: double.infinity,
                  child: Text(
                    'A Jellyfin Client for Wii',
                    textAlign: TextAlign.center,
                    style: TextStyle(
                      fontSize: 54,
                      fontWeight: FontWeight.w600,
                      color: Colors.black87,
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
        // Partie inférieure : fond "lignes" + boutons centrés
        SizedBox(
          height: isMobile ? 260 : 180, // plus haut sur mobile
          width: double.infinity,
          child: Stack(
            children: [
              const Positioned.fill(
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    image: DecorationImage(
                      image: AssetImage('assets/lines.png'),
                      fit: BoxFit.cover,
                    ),
                  ),
                ),
              ),
              Center(
                child: WiiBottomBar(isMobile: isMobile),
              ),
            ],
          ),
        ),
      ],
    );
  }
}

class _CustomCursor extends StatelessWidget {
  const _CustomCursor();

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<Offset>(
      valueListenable: WiiFinHomePage.cursorPosition,
      builder: (context, pos, child) {
        // Pour forcer la taille, vérifie que le cache n'est pas en cause :
        // Ajoute un key unique à l'image pour forcer le rebuild
        return Stack(
          children: [
            Positioned(
              left: pos.dx,
              top: pos.dy,
              child: IgnorePointer(
                child: Image.asset(
                  'assets/cursor.png',
                  key: ValueKey('${pos.dx}_${pos.dy}'), // force le rebuild de l'image
                  width: 80,   // ⬅️ ENCORE PLUS GRAND
                  height: 80,  // ⬅️ ENCORE PLUS GRAND
                  fit: BoxFit.contain,
                ),
              ),
            ),
          ],
        );
      },
    );
  }
}

class WiiBottomBar extends StatelessWidget {
  final bool isMobile;
  const WiiBottomBar({super.key, this.isMobile = false});

  @override
  Widget build(BuildContext context) {
    final screenWidth = MediaQuery.of(context).size.width;
    if (isMobile) {
      return Column(
        mainAxisAlignment: MainAxisAlignment.start,
        children: [
          const SizedBox(height: 32), // remonte les boutons
          WiiButton(label: 'Github', width: screenWidth * 0.7),
          const SizedBox(height: 20),
          WiiButton(label: 'Discord', width: screenWidth * 0.7),
          const Spacer(), // pousse vers le haut
        ],
      );
    }
    return Row(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        WiiButton(label: 'Github', width: screenWidth * 0.22),
        const SizedBox(width: 56),
        WiiButton(label: 'Discord', width: screenWidth * 0.22),
      ],
    );
  }
}

class WiiButton extends StatefulWidget {
  final String label;
  final double? width;
  const WiiButton({super.key, required this.label, this.width});

  @override
  State<WiiButton> createState() => _WiiButtonState();
}

class _WiiButtonState extends State<WiiButton> {
  bool _hovering = false;

  void _handleTap() async {
    String? url;
    if (widget.label == 'Github') {
      url = 'https://github.com/fabienmillet/WiiFin';
    } else if (widget.label == 'Discord') {
      url = 'https://discord.gg/p9DXfEmUYu';
    }
    if (url != null) {
      final uri = Uri.parse(url);
      if (await canLaunchUrl(uri)) {
        await launchUrl(uri, mode: LaunchMode.externalApplication);
      } else {
        debugPrint('Could not launch $url');
      }
    } else {
      debugPrint('${widget.label} clicked!');
    }
  }

  @override
  Widget build(BuildContext context) {
    final buttonWidth = widget.width ?? 380;
    return MouseRegion(
      onEnter: (_) => setState(() => _hovering = true),
      onExit: (_) => setState(() => _hovering = false),
      child: GestureDetector(
        onTap: _handleTap,
        child: AnimatedScale(
          scale: _hovering ? 1.1 : 1.0,
          duration: const Duration(milliseconds: 150),
          child: Stack(
            alignment: Alignment.center,
            children: [
              Image.asset(
                'assets/button.png',
                width: buttonWidth.clamp(300, 500),
              ),
              Text(
                widget.label,
                style: const TextStyle(
                  fontSize: 36, // encore plus grand
                  fontWeight: FontWeight.w600,
                  color: Colors.black87,
                ),
              )
            ],
          ),
        ),
      ),
    );
  }
}