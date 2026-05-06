import 'dart:async';
import 'package:flutter/material.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_database/firebase_database.dart';
import 'package:geolocator/geolocator.dart';
import 'package:google_maps_flutter/google_maps_flutter.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:permission_handler/permission_handler.dart';
import 'background_service.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp();
  await Permission.notification.request();
  await Permission.locationAlways.request();
  await Permission.ignoreBatteryOptimizations.request();
  await initializeBackgroundService();
  runApp(const MicroFluxApp());
}

class MicroFluxApp extends StatelessWidget {
  const MicroFluxApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorSchemeSeed: const Color(0xFF1A237E),
        useMaterial3: true,
      ),
      home: const SplashScreen(),
    );
  }
}

// ── SPLASH SCREEN ───────────────────────────────────────────────────────────────
class SplashScreen extends StatefulWidget {
  const SplashScreen({super.key});

  @override
  State<SplashScreen> createState() => _SplashState();
}

class _SplashState extends State<SplashScreen> {
  @override
  void initState() {
    super.initState();
    _checkStatus();
  }

  Future<void> _checkStatus() async {
    Timer(const Duration(seconds: 3), () {
      if (!mounted) return;
      Navigator.pushReplacement(
        context,
        MaterialPageRoute(
          builder: (_) => const RegisterScreen(),
        ),
      );
    });
  }

  @override
  Widget build(BuildContext context) {
    return const Scaffold(
      backgroundColor: Color(0xFF1A237E),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(Icons.security, size: 100, color: Colors.white),
            SizedBox(height: 20),
            Text(
              "MICROFLUX",
              style: TextStyle(
                color: Colors.white,
                fontSize: 32,
                fontWeight: FontWeight.bold,
                letterSpacing: 4,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ── REGISTER SCREEN ─────────────────────────────────────────────────────────────
class RegisterScreen extends StatefulWidget {
  const RegisterScreen({super.key});

  @override
  State<RegisterScreen> createState() => _RegisterState();
}

class _RegisterState extends State<RegisterScreen> {
  final _simController = TextEditingController();
  final _ssidController = TextEditingController();
  final _passController = TextEditingController();
  bool _isLoading = false;

  @override
  void initState() {
    super.initState();
    _loadSavedData();
  }

  Future<void> _loadSavedData() async {
    final prefs = await SharedPreferences.getInstance();
    setState(() {
      _simController.text = prefs.getString('sim_number') ?? '';
      _ssidController.text = prefs.getString('ssid') ?? '';
      _passController.text = prefs.getString('wifi_password') ?? '';
    });
  }

  Future<void> _saveAndGo() async {
    final String sim = _simController.text.trim();
    final String ssid = _ssidController.text.trim();
    final String pass = _passController.text.trim();
    if (sim.isEmpty || ssid.isEmpty || pass.isEmpty) return;

    setState(() => _isLoading = true);

    try {
      final db = FirebaseDatabase.instance.ref();
      await db.child("config/network").set({
        "sim": sim,
        "ssid": ssid,
        "password": pass,
      });

      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('sim_number', sim);
      await prefs.setString('ssid', ssid);
      await prefs.setString('wifi_password', pass);

      if (!mounted) return;
      Navigator.pushReplacement(
        context,
        MaterialPageRoute(builder: (_) => const Dashboard()),
      );
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text("Error saving: $e")),
      );
      setState(() => _isLoading = false);
    }
  }

  Widget _buildGlassField(String label, TextEditingController controller, {bool obscure = false, TextInputType? type}) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(label, style: const TextStyle(color: Colors.white, fontSize: 12, letterSpacing: 2, fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        Container(
          decoration: BoxDecoration(
            color: Colors.white.withOpacity(0.15),
            borderRadius: BorderRadius.circular(15),
          ),
          child: TextField(
            controller: controller,
            obscureText: obscure,
            keyboardType: type,
            style: const TextStyle(color: Colors.white),
            decoration: const InputDecoration(
              border: InputBorder.none,
              contentPadding: EdgeInsets.symmetric(horizontal: 20, vertical: 15),
            ),
          ),
        ),
        const SizedBox(height: 20),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Container(
        decoration: const BoxDecoration(
          gradient: LinearGradient(
            colors: [Color(0xFF0F172A), Color(0xFF5A189A)],
            begin: Alignment.topRight,
            end: Alignment.bottomLeft,
          ),
        ),
        child: SafeArea(
          child: Center(
            child: SingleChildScrollView(
              padding: const EdgeInsets.symmetric(horizontal: 40.0),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  const Text(
                    "Connect\nDevice",
                    textAlign: TextAlign.center,
                    style: TextStyle(
                      color: Colors.white,
                      fontSize: 40,
                      fontWeight: FontWeight.w900,
                      height: 1.1,
                    ),
                  ),
                  const SizedBox(height: 10),
                  const Text(
                    "Setup your A9G module connection",
                    style: TextStyle(color: Colors.white70, fontSize: 14),
                  ),
                  const SizedBox(height: 40),
                  _buildGlassField("A9G SIM NUMBER", _simController, type: TextInputType.phone),
                  _buildGlassField("WIFI SSID", _ssidController),
                  _buildGlassField("WIFI PASSWORD", _passController, obscure: true),
                  const SizedBox(height: 10),
                  SizedBox(
                    width: double.infinity,
                    height: 55,
                    child: ElevatedButton(
                      onPressed: _isLoading ? null : _saveAndGo,
                      style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.transparent,
                        shadowColor: Colors.transparent,
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(30),
                          side: const BorderSide(color: Colors.white, width: 2),
                        ),
                      ),
                      child: _isLoading 
                          ? const CircularProgressIndicator(color: Colors.white) 
                          : const Text("Start Tracking", style: TextStyle(color: Colors.white, fontSize: 16, fontWeight: FontWeight.bold)),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

// ── DASHBOARD ───────────────────────────────────────────────────────────────────
class Dashboard extends StatefulWidget {
  const Dashboard({super.key});

  @override
  State<Dashboard> createState() => _DashState();
}

class _DashState extends State<Dashboard> {
  final _db = FirebaseDatabase.instance.ref();
  LatLng? _phone, _bag;
  double _dist = 0;
  double _limit = 20.0;
  bool _ringcut = false;
  bool _led = false;
  bool _buzzer = false;
  bool _playMusicMode = true;
  String _status = "🛡️ System Secure";
  int _batteryLevel = 100;

  @override
  void initState() {
    super.initState();
    _loadPrefs();
    _startTracking();
  }

  Future<void> _loadPrefs() async {
    final prefs = await SharedPreferences.getInstance();
    setState(() {
      _limit = prefs.getDouble('threshold') ?? 20.0;
      _playMusicMode = prefs.getBool('play_music') ?? true;
    });
  }

  // Local notifications handled by background service

  Future<void> _startTracking() async {
    bool permission =
        await Geolocator.requestPermission() == LocationPermission.always ||
        await Geolocator.requestPermission() == LocationPermission.whileInUse;

    if (!permission) return;

    Geolocator.getPositionStream(
      locationSettings: const LocationSettings(
        accuracy: LocationAccuracy.high,
        distanceFilter: 2,
      ),
    ).listen((Position p) {
      if (!mounted) return;
      setState(() => _phone = LatLng(p.latitude, p.longitude));
      _updateLogic();
    });

    _db.child("bag_data/bag_location").onValue.listen((event) {
      if (!mounted) return;
      final data = event.snapshot.value;
      if (data == null) return;

      final d = Map<dynamic, dynamic>.from(data as Map);
      setState(() {
        _bag = LatLng(
          double.tryParse(d['lat']?.toString() ?? '0') ?? 0,
          double.tryParse(d['lon']?.toString() ?? '0') ?? 0,
        );
      });
      _updateLogic();
    });

    _db.child("bag_data/battery").onValue.listen((event) {
      if (!mounted) return;
      if (event.snapshot.value != null) {
        setState(() {
          _batteryLevel = int.tryParse(event.snapshot.value.toString()) ?? 0;
        });
      }
    });
  }

  void _updateLogic() {
    if (_phone == null || _bag == null) return;

    _dist = Geolocator.distanceBetween(
      _phone!.latitude,
      _phone!.longitude,
      _bag!.latitude,
      _bag!.longitude,
    );

    final bool inDanger = _dist > _limit;
    _status = inDanger ? "⚠️ PROXIMITY VIOLATION" : "🛡️ System Secure";

    if (mounted) setState(() {});
    
    // Throttle automatic Firebase updates to prevent massive slowdowns
    if (_lastAlarm != inDanger || (_lastDist - _dist).abs() > 5 || _lastLimit != _limit.toInt()) {
      _lastAlarm = inDanger;
      _lastDist = _dist;
      _lastLimit = _limit.toInt();
      _sendCommandsToFirebase();
    }
  }

  bool _lastAlarm = false;
  double _lastDist = -1;
  int _lastLimit = -1;

  void _sendCommandsToFirebase() {
    _db.child("commands").update({
      "rc": _ringcut,
      "l": _led,
      "b": _buzzer,
      "alarm": _dist > _limit,
      "msg": _status,
      "dist": _dist.toStringAsFixed(1),
      "threshold": _limit.toInt(),
    });
  }

  void _sendSmsCommand() {
    _db.child("commands").update({"send_sms_location": true});
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text("SMS Location command sent to module!")),
    );
  }

  void _showLimitDialog() {
    double tempLimit = _limit;
    showDialog(
      context: context,
      builder: (context) {
        return StatefulBuilder(
          builder: (context, setDialogState) {
            return AlertDialog(
              title: const Text("Set Distance Limit"),
              content: SizedBox(
                height: 100,
                child: Column(
                  children: [
                    Text("${tempLimit.toInt()} m", style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold)),
                    Slider(
                      value: tempLimit,
                      min: 2,
                      max: 300,
                      divisions: 298,
                      onChanged: (v) {
                        setDialogState(() => tempLimit = v);
                      },
                    ),
                  ],
                ),
              ),
              actions: [
                TextButton(onPressed: () => Navigator.pop(context), child: const Text("CANCEL")),
                TextButton(
                  onPressed: () async {
                    setState(() => _limit = tempLimit);
                    final prefs = await SharedPreferences.getInstance();
                    await prefs.setDouble('threshold', _limit);
                    _updateLogic();
                    Navigator.pop(context);
                  }, 
                  child: const Text("SAVE")
                ),
              ],
            );
          }
        );
      }
    );
  }

  Widget _buildStatItem(IconData icon, String value, String label, {VoidCallback? onTap}) {
    return GestureDetector(
      onTap: onTap,
      child: Column(
        children: [
          Icon(icon, color: Colors.white70, size: 24),
          const SizedBox(height: 4),
          Text(value, style: const TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.bold)),
          Text(label, style: const TextStyle(color: Colors.white70, fontSize: 12)),
        ],
      ),
    );
  }

  Widget _buildGridItem(String title, IconData icon, Color color, VoidCallback onTap, {bool isActive = false}) {
    return GestureDetector(
      onTap: onTap,
      child: Container(
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(20),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withOpacity(0.05),
              blurRadius: 10,
              offset: const Offset(0, 5),
            )
          ]
        ),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            CircleAvatar(
              backgroundColor: isActive ? color.withOpacity(0.2) : Colors.grey.shade100,
              radius: 25,
              child: Icon(icon, color: isActive ? color : Colors.grey.shade600, size: 28),
            ),
            const SizedBox(height: 12),
            Text(title, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 13)),
            const SizedBox(height: 8),
            Container(
              height: 3,
              width: 30,
              decoration: BoxDecoration(
                color: isActive ? color : Colors.grey.shade300,
                borderRadius: BorderRadius.circular(2),
              ),
            )
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    bool inDanger = _dist > _limit;

    return Scaffold(
      backgroundColor: Colors.grey.shade50,
      body: Column(
        children: [
          Container(
            width: double.infinity,
            padding: const EdgeInsets.only(top: 60, bottom: 30, left: 20, right: 20),
            decoration: BoxDecoration(
              gradient: LinearGradient(
                colors: inDanger 
                    ? [const Color(0xFFE53935), const Color(0xFFB71C1C)]
                    : [const Color(0xFFFFB75E), const Color(0xFFED8F03)],
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
              ),
              borderRadius: const BorderRadius.only(
                bottomLeft: Radius.circular(40),
                bottomRight: Radius.circular(40),
              )
            ),
            child: Column(
              children: [
                Text(
                  inDanger ? "PROXIMITY ALERT" : "SYSTEM SECURE",
                  style: const TextStyle(color: Colors.white, fontSize: 22, fontWeight: FontWeight.bold, letterSpacing: 1.5),
                ),
                const SizedBox(height: 20),
                BatteryGauge(percentage: _batteryLevel),
                const SizedBox(height: 20),
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                  children: [
                    _buildStatItem(Icons.social_distance, "${_dist.toInt()} m", "Distance"),
                    _buildStatItem(Icons.tune, "${_limit.toInt()} m", "Limit", onTap: _showLimitDialog),
                  ],
                )
              ],
            ),
          ),
          Expanded(
            child: GridView.count(
              padding: const EdgeInsets.all(20),
              crossAxisCount: 2,
              crossAxisSpacing: 15,
              mainAxisSpacing: 15,
              childAspectRatio: 1.1,
              children: [
                _buildGridItem("LED Light", Icons.lightbulb, Colors.orange, () {
                  setState(() => _led = !_led);
                  _sendCommandsToFirebase();
                }, isActive: _led),
                _buildGridItem("Buzzer", Icons.volume_up, Colors.blue, () {
                  setState(() => _buzzer = !_buzzer);
                  _sendCommandsToFirebase();
                }, isActive: _buzzer),
                _buildGridItem("Ringcut", Icons.power_settings_new, Colors.red, () {
                  setState(() => _ringcut = !_ringcut);
                  _sendCommandsToFirebase();
                }, isActive: _ringcut),
                _buildGridItem("Phone Alarm", Icons.music_note, Colors.purple, () async {
                  setState(() => _playMusicMode = !_playMusicMode);
                  final prefs = await SharedPreferences.getInstance();
                  await prefs.setBool('play_music', _playMusicMode);
                }, isActive: _playMusicMode),
                _buildGridItem("GPS Map", Icons.map, Colors.green, () {
                  Navigator.push(context, MaterialPageRoute(builder: (_) => MapScreen(bagLoc: _bag, phoneLoc: _phone, dist: _dist)));
                }, isActive: true),
                _buildGridItem("SMS Location", Icons.sms, Colors.teal, () {
                  _sendSmsCommand();
                }, isActive: true),
              ],
            ),
          ),
        ],
      ),
    );
  }

  @override
  void dispose() {
    super.dispose();
  }
}

class BatteryGauge extends StatelessWidget {
  final int percentage;

  const BatteryGauge({super.key, required this.percentage});

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 150,
      height: 75,
      child: CustomPaint(
        painter: _BatteryPainter(percentage),
        child: Center(
          child: Padding(
            padding: const EdgeInsets.only(top: 20.0),
            child: Text(
              '$percentage%',
              style: const TextStyle(
                color: Colors.white,
                fontSize: 24,
                fontWeight: FontWeight.bold,
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _BatteryPainter extends CustomPainter {
  final int percentage;
  _BatteryPainter(this.percentage);

  @override
  void paint(Canvas canvas, Size size) {
    Paint bgPaint = Paint()
      ..color = Colors.white.withOpacity(0.3)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 10
      ..strokeCap = StrokeCap.round;

    Color fgColor = percentage < 20 ? Colors.red : Colors.white;

    Paint fgPaint = Paint()
      ..color = fgColor
      ..style = PaintingStyle.stroke
      ..strokeWidth = 10
      ..strokeCap = StrokeCap.round;

    Rect rect = Rect.fromCenter(center: Offset(size.width / 2, size.height), width: size.width, height: size.height * 2);
    
    // Draw background arc
    canvas.drawArc(rect, 3.14159, 3.14159, false, bgPaint);
    
    // Draw foreground arc
    double sweepAngle = (percentage / 100) * 3.14159;
    canvas.drawArc(rect, 3.14159, sweepAngle, false, fgPaint);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => true;
}

class MapScreen extends StatelessWidget {
  final LatLng? bagLoc;
  final LatLng? phoneLoc;
  final double dist;

  const MapScreen({super.key, this.bagLoc, this.phoneLoc, required this.dist});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text("GPS Tracking Map"),
        backgroundColor: const Color(0xFFFFB75E),
        foregroundColor: Colors.white,
      ),
      body: bagLoc == null
          ? const Center(child: CircularProgressIndicator())
          : GoogleMap(
              initialCameraPosition: CameraPosition(
                target: bagLoc!,
                zoom: 17,
              ),
              markers: {
                Marker(
                  markerId: const MarkerId("bag"),
                  position: bagLoc!,
                  icon: BitmapDescriptor.defaultMarkerWithHue(BitmapDescriptor.hueRed),
                ),
              },
              myLocationEnabled: true,
              myLocationButtonEnabled: true,
            ),
    );
  }
}
