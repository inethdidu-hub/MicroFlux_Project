import 'dart:async';
import 'package:flutter/material.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_database/firebase_database.dart';
import 'package:geolocator/geolocator.dart';
import 'package:google_maps_flutter/google_maps_flutter.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:url_launcher/url_launcher.dart';
import 'background_service.dart';
import 'package:flutter_background_service/flutter_background_service.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp();
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
    final prefs = await SharedPreferences.getInstance();
    final String sim = prefs.getString('sim_number') ?? '';
    final String ssid = prefs.getString('ssid') ?? '';

    Timer(const Duration(seconds: 3), () async {
      if (!mounted) return;
      if (sim.isNotEmpty && ssid.isNotEmpty) {
        // Request permissions and start service
        await Permission.notification.request();
        await Permission.locationAlways.request();
        await Permission.ignoreBatteryOptimizations.request();
        
        LocationPermission permission = await Geolocator.checkPermission();
        if (permission == LocationPermission.denied) {
          permission = await Geolocator.requestPermission();
        }
        if (permission == LocationPermission.always || permission == LocationPermission.whileInUse) {
          await initializeBackgroundService();
        }

        if (!mounted) return;
        Navigator.pushReplacement(
          context,
          MaterialPageRoute(builder: (_) => const Dashboard()),
        );
      } else {
        if (!mounted) return;
        Navigator.pushReplacement(
          context,
          MaterialPageRoute(builder: (_) => const RegisterScreen()),
        );
      }
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
  final _ownerPhoneController = TextEditingController();
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
      _ownerPhoneController.text = prefs.getString('owner_phone') ?? '';
      _ssidController.text = prefs.getString('ssid') ?? '';
      _passController.text = prefs.getString('wifi_password') ?? '';
    });
  }

  Future<void> _saveAndGo() async {
    final String sim = _simController.text.trim();
    final String ownerPhone = _ownerPhoneController.text.trim();
    final String ssid = _ssidController.text.trim();
    final String pass = _passController.text.trim();
    if (sim.isEmpty || ownerPhone.isEmpty || ssid.isEmpty || pass.isEmpty) return;

    setState(() => _isLoading = true);

    try {
      // Automatically fetch current phone location for Wi-Fi zone mapping
      double phoneLat = 0.0;
      double phoneLon = 0.0;
      try {
        LocationPermission permission = await Geolocator.checkPermission();
        if (permission == LocationPermission.denied) {
          permission = await Geolocator.requestPermission();
        }
        if (permission == LocationPermission.always || permission == LocationPermission.whileInUse) {
          // Fast check using last known position first (instant)
          Position? position = await Geolocator.getLastKnownPosition();
          if (position == null) {
            // Low-accuracy fast check with a 2-second timeout as a fallback
            position = await Geolocator.getCurrentPosition(
              desiredAccuracy: LocationAccuracy.low,
              timeLimit: const Duration(seconds: 2),
            );
          }
          phoneLat = position.latitude;
          phoneLon = position.longitude;
        }
      } catch (e) {
        debugPrint("Error fetching phone GPS location: $e");
      }

      final db = FirebaseDatabase.instance.ref();
      await db.child("config/network").set({
        "sim": sim,
        "ssid": ssid,
        "password": pass,
      });

      await db.child("commands").update({
        "owner_phone": ownerPhone,
      });

      if (phoneLat != 0.0 && phoneLon != 0.0) {
        await db.child("commands").update({
          "wifi_lat": phoneLat,
          "wifi_lon": phoneLon,
        });
      }

      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('sim_number', sim);
      await prefs.setString('owner_phone', ownerPhone);
      await prefs.setString('ssid', ssid);
      await prefs.setString('wifi_password', pass);

      // Request permissions and start service
      await Permission.notification.request();
      await Permission.locationAlways.request();
      await Permission.ignoreBatteryOptimizations.request();

      LocationPermission permission = await Geolocator.checkPermission();
      if (permission == LocationPermission.denied) {
        permission = await Geolocator.requestPermission();
      }
      if (permission == LocationPermission.always || permission == LocationPermission.whileInUse) {
        await initializeBackgroundService();
      }

      // Navigate to Dashboard immediately first so it is ready when user returns
      if (!mounted) return;
      Navigator.pushReplacement(
        context,
        MaterialPageRoute(builder: (_) => const Dashboard()),
      );

      // Launch SMS configuration composer in background asynchronously
      final Uri smsUri = Uri.parse('sms:$sim?body=WIFI:$ssid,$pass');
      try {
        canLaunchUrl(smsUri).then((canLaunch) {
          if (canLaunch) {
            launchUrl(smsUri);
          }
        });
      } catch (e) {
        debugPrint("Error launching SMS composer: $e");
      }
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
                  _buildGlassField("OWNER PHONE NUMBER", _ownerPhoneController, type: TextInputType.phone),
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
  double _wifiDist = -1.0;
  double _limit = 20.0;

  bool _led = false;
  bool _buzzer = false;
  bool _playMusicMode = true;
  String _status = "🛡️ System Secure";
  int _batteryLevel = 100;
  int _csqVal = 0;
  bool _lbs = false;
  bool _gpsLocked = false;

  bool _tamper = false;
  bool _isRinging = false;
  double _alarmVolume = 1.0;
  double? _bagLat;
  double? _bagLon;

  final List<StreamSubscription> _subscriptions = [];

  void _cancelSubscriptions() {
    for (var sub in _subscriptions) {
      sub.cancel();
    }
    _subscriptions.clear();
  }

  @override
  void dispose() {
    _cancelSubscriptions();
    super.dispose();
  }

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
      _alarmVolume = prefs.getDouble('alarm_volume') ?? 1.0;
    });
  }

  Future<void> _startTracking() async {
    LocationPermission permission = await Geolocator.checkPermission();
    if (permission == LocationPermission.denied) {
      permission = await Geolocator.requestPermission();
    }
    if (permission == LocationPermission.denied ||
        permission == LocationPermission.deniedForever) {
      return;
    }

    // Instantly get last known position to resolve null state under 10ms
    try {
      Position? lastPos = await Geolocator.getLastKnownPosition();
      if (lastPos != null && mounted) {
        setState(() => _phone = LatLng(lastPos.latitude, lastPos.longitude));
        _updateLogic();
      }
    } catch (_) {}

    _subscriptions.add(
      Geolocator.getPositionStream(
        locationSettings: const LocationSettings(
          accuracy: LocationAccuracy.high,
          distanceFilter: 2,
        ),
      ).listen((Position p) {
        if (!mounted) return;
        setState(() => _phone = LatLng(p.latitude, p.longitude));
        _updateLogic();
      })
    );

    _subscriptions.add(
      _db.child("bag_data/lat").onValue.listen((event) {
        if (!mounted) return;
        final val = event.snapshot.value;
        if (val != null) {
          _bagLat = double.tryParse(val.toString());
          _updateBagLatLng();
        }
      })
    );

    _subscriptions.add(
      _db.child("bag_data/lon").onValue.listen((event) {
        if (!mounted) return;
        final val = event.snapshot.value;
        if (val != null) {
          _bagLon = double.tryParse(val.toString());
          _updateBagLatLng();
        }
      })
    );

    _subscriptions.add(
      _db.child("bag_data/battery").onValue.listen((event) {
        if (!mounted) return;
        if (event.snapshot.value != null) {
          setState(() {
            _batteryLevel = int.tryParse(event.snapshot.value.toString()) ?? 0;
          });
        }
      })
    );

    _subscriptions.add(
      _db.child("bag_data/tamper").onValue.listen((event) {
        if (!mounted) return;
        final val = event.snapshot.value;
        setState(() {
          _tamper = (val == true || val == "true" || val == 1);
        });
        _updateLogic();
      })
    );

    _subscriptions.add(
      _db.child("bag_data/lbs").onValue.listen((event) {
        if (!mounted) return;
        final val = event.snapshot.value;
        setState(() {
          _lbs = (val == true || val == "true" || val == 1);
        });
        _updateLogic();
      })
    );

    _subscriptions.add(
      _db.child("bag_data/wifi_dist").onValue.listen((event) {
        if (!mounted) return;
        final val = event.snapshot.value;
        if (val != null) {
          setState(() {
            _wifiDist = double.tryParse(val.toString()) ?? -1.0;
          });
          _updateLogic();
        }
      })
    );

    _subscriptions.add(
      _db.child("bag_data/gps_locked").onValue.listen((event) {
        if (!mounted) return;
        final val = event.snapshot.value;
        setState(() {
          _gpsLocked = (val == true || val == "true" || val == 1);
        });
        _updateLogic();
      })
    );

    _subscriptions.add(
      _db.child("bag_data/csq").onValue.listen((event) {
        if (!mounted) return;
        if (event.snapshot.value != null) {
          setState(() {
            _csqVal = int.tryParse(event.snapshot.value.toString()) ?? 0;
          });
        }
      })
    );

    _subscriptions.add(
      FlutterBackgroundService().on('ringing_status').listen((event) {
        if (event != null && mounted) {
          setState(() {
            _isRinging = event['ringing'] == true;
          });
        }
      })
    );
  }

  void _updateBagLatLng() {
    if (_bagLat != null && _bagLon != null) {
      setState(() {
        _bag = LatLng(_bagLat!, _bagLon!);
      });
      _updateLogic();
    }
  }

  void _updateLogic() {
    // If Wi-Fi distance is active, update and check alarm conditions instantly (even if GPS is null)
    if (_wifiDist > 0.0) {
      _dist = _wifiDist;
      final bool inDanger = _dist > _limit || _tamper;
      _status = _tamper 
          ? "🚨 DEVICE REMOVED" 
          : (inDanger ? "⚠️ PROXIMITY VIOLATION" : "🛡️ System Secure (Wi-Fi Zone)");
      if (mounted) setState(() {});
      
      if (_lastAlarm != inDanger || (_lastDist - _dist).abs() > 0.5 || _lastLimit != _limit.toInt()) {
        _lastAlarm = inDanger;
        _lastDist = _dist;
        _lastLimit = _limit.toInt();
        _sendCommandsToFirebase();
      }
      return;
    }

    if (_phone == null || _bag == null) return;

    bool isGPSInvalid = (_bag!.latitude == 0.0 && _bag!.longitude == 0.0);
    bool isLbs = _lbs == true;

    if (isGPSInvalid) {
      setState(() {
        _status = isLbs ? "LBS Location Active" : "Connecting to GPS...";
        _dist = 0.0;
      });
      _sendCommandsToFirebase();
      return;
    }

    _dist = Geolocator.distanceBetween(
      _phone!.latitude,
      _phone!.longitude,
      _bag!.latitude,
      _bag!.longitude,
    );

    final bool inDanger = _dist > _limit || _tamper;
    _status = _tamper 
        ? "🚨 DEVICE REMOVED" 
        : (inDanger ? "⚠️ PROXIMITY VIOLATION" : "🛡️ System Secure");

    if (mounted) setState(() {});
    
    if (_lastAlarm != inDanger || (_lastDist - _dist).abs() > 0.5 || _lastLimit != _limit.toInt()) {
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
      "l": _led,
      "b": _buzzer,
      "alarm": (_dist > _limit || _tamper),
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
                    FlutterBackgroundService().invoke('set_threshold', {'threshold': _limit});
                    _updateLogic();
                    Navigator.pop(context);
                  }, 
                  child: const Text("SAVE")
                ),
              ],
            );
          },
        );
      },
    );
  }

  void _showSignOutDialog() {
    showDialog(
      context: context,
      builder: (context) {
        return AlertDialog(
          title: const Text("Sign Out & Reset"),
          content: const Text(
            "Are you sure you want to sign out? This will clear settings from the app and prompt you to send a WIFI:RESET SMS to reset the tracker's Wi-Fi."
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text("CANCEL"),
            ),
            TextButton(
              onPressed: () async {
                Navigator.pop(context);
                await _performSignOut();
              },
              child: const Text("SIGN OUT", style: TextStyle(color: Colors.red)),
            ),
          ],
        );
      },
    );
  }

  Future<void> _performSignOut() async {
    _cancelSubscriptions();
    final prefs = await SharedPreferences.getInstance();
    final String sim = prefs.getString('sim_number') ?? '';
    
    // Clear SharedPreferences
    await prefs.remove('sim_number');
    await prefs.remove('owner_phone');
    await prefs.remove('ssid');
    await prefs.remove('wifi_password');
    
    // Stop background service
    try {
      final service = FlutterBackgroundService();
      if (await service.isRunning()) {
        service.invoke("stop_service");
      }
    } catch (e) {
      debugPrint("Error stopping service: $e");
    }
    
    // Send WIFI:RESET SMS to module
    if (sim.isNotEmpty) {
      final Uri smsUri = Uri.parse('sms:$sim?body=WIFI:RESET');
      try {
        if (await canLaunchUrl(smsUri)) {
          await launchUrl(smsUri);
        }
      } catch (e) {
        debugPrint("Error launching SMS: $e");
      }
    }
    
    if (!mounted) return;
    Navigator.pushReplacement(
      context,
      MaterialPageRoute(builder: (_) => const RegisterScreen()),
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
    bool inDanger = _dist > _limit || _tamper;

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
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    const SizedBox(width: 48), // Spacer to balance logout icon button
                    Expanded(
                      child: Text(
                        (_bag == null || (_bag!.latitude == 0.0 && _bag!.longitude == 0.0))
                            ? (_lbs ? "LBS LOCATION ACTIVE" : "CONNECTING TO GPS...")
                            : (_tamper ? "DEVICE REMOVED" : (inDanger ? "PROXIMITY ALERT" : "SYSTEM SECURE")),
                        textAlign: TextAlign.center,
                        style: const TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.bold, letterSpacing: 1.5),
                      ),
                    ),
                    IconButton(
                      icon: const Icon(Icons.logout, color: Colors.white),
                      tooltip: "Sign Out & Reset Wi-Fi",
                      onPressed: _showSignOutDialog,
                    ),
                  ],
                ),
                const SizedBox(height: 20),
                BatteryGauge(percentage: _batteryLevel),
                const SizedBox(height: 20),
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                  children: [
                    _buildStatItem(
                      Icons.social_distance,
                      _wifiDist > 0.0 ? "${_dist.toStringAsFixed(1)} m" : "${_dist.toInt()} m",
                      "Distance",
                    ),
                    _buildStatItem(Icons.tune, "${_limit.toInt()} m", "Limit", onTap: _showLimitDialog),
                  ],
                ),
                if (_isRinging) ...[
                  const SizedBox(height: 15),
                  ElevatedButton.icon(
                    onPressed: () {
                      FlutterBackgroundService().invoke("mute_alarm");
                      setState(() {
                        _isRinging = false;
                      });
                    },
                    icon: const Icon(Icons.volume_off, color: Colors.red),
                    label: const Text(
                      "MUTE PHONE ALARM", 
                      style: TextStyle(color: Colors.red, fontWeight: FontWeight.bold, letterSpacing: 1)
                    ),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.white,
                      padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(30),
                      ),
                    ),
                  ),
                ],
              ],
            ),
          ),
          Expanded(
            child: SingleChildScrollView(
              padding: const EdgeInsets.all(20),
              child: Column(
                children: [
                  // --- DEVICE REMOVAL STATUS CARD ---
                  (_bag == null || (_bag!.latitude == 0.0 && _bag!.longitude == 0.0))
                  ? Container(
                      width: double.infinity,
                      padding: const EdgeInsets.all(20),
                      margin: const EdgeInsets.only(bottom: 15),
                      decoration: BoxDecoration(
                        color: Colors.grey[100],
                        borderRadius: BorderRadius.circular(20),
                        border: Border.all(
                          color: Colors.grey[350]!,
                          width: 1.5,
                        ),
                      ),
                      child: Row(
                        children: [
                          Icon(
                            Icons.cloud_off,
                            color: Colors.grey[600],
                            size: 36,
                          ),
                          const SizedBox(width: 15),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text(
                                  "Device Connection Status",
                                  style: TextStyle(
                                    fontWeight: FontWeight.bold,
                                    fontSize: 16,
                                    color: Colors.grey[800],
                                  ),
                                ),
                                const SizedBox(height: 4),
                                const Text(
                                  "Tracking module is offline. Awaiting connection...",
                                  style: TextStyle(
                                    fontSize: 13,
                                    color: Colors.black54,
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ],
                      ),
                    )
                  : Container(
                      width: double.infinity,
                      padding: const EdgeInsets.all(20),
                      margin: const EdgeInsets.only(bottom: 15),
                      decoration: BoxDecoration(
                        color: _tamper ? const Color(0xFFFFEBEE) : const Color(0xFFE8F5E9),
                        borderRadius: BorderRadius.circular(20),
                        border: Border.all(
                          color: _tamper ? const Color(0xFFEF5350) : const Color(0xFF66BB6A),
                          width: 1.5,
                        ),
                      ),
                      child: Row(
                        children: [
                          Icon(
                            _tamper ? Icons.report_problem : Icons.gpp_good,
                            color: _tamper ? const Color(0xFFD32F2F) : const Color(0xFF388E3C),
                            size: 36,
                          ),
                          const SizedBox(width: 15),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text(
                                  "Device Removal Status",
                                  style: TextStyle(
                                    fontWeight: FontWeight.bold,
                                    fontSize: 16,
                                    color: _tamper ? const Color(0xFFB71C1C) : const Color(0xFF1B5E20),
                                  ),
                                ),
                                const SizedBox(height: 4),
                                Text(
                                  _tamper 
                                      ? "Your device has been removed from your bag" 
                                      : "The tracking module is securely inside the bag.",
                                  style: TextStyle(
                                    fontSize: 13,
                                    color: _tamper ? const Color(0xFFC62828) : const Color(0xFF2E7D32),
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ],
                      ),
                    ),

                  // --- GPS & BATTERY TELEMETRY CARD ---
                  Container(
                    width: double.infinity,
                    padding: const EdgeInsets.all(20),
                    margin: const EdgeInsets.only(bottom: 15),
                    decoration: BoxDecoration(
                      color: Colors.white,
                      borderRadius: BorderRadius.circular(20),
                      boxShadow: [
                        BoxShadow(
                          color: Colors.black.withOpacity(0.05),
                          blurRadius: 10,
                          offset: const Offset(0, 5),
                        )
                      ],
                    ),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text(
                          "Tracking Module Status",
                          style: TextStyle(
                            fontWeight: FontWeight.bold,
                            fontSize: 15,
                            color: Colors.black87,
                          ),
                        ),
                        const SizedBox(height: 15),
                        // GPS Fix section
                        Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Icon(
                              _gpsLocked
                                  ? Icons.gps_fixed
                                  : (_lbs ? Icons.cell_tower : Icons.gps_off),
                              color: _gpsLocked
                                  ? Colors.green
                                  : (_lbs ? Colors.blue : Colors.orange),
                              size: 24,
                            ),
                            const SizedBox(width: 15),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text(
                                    _gpsLocked
                                        ? "GPS Status: Satellite Lock"
                                        : (_lbs ? "GPS Status: LBS Coarse Fix" : "GPS Status: Searching..."),
                                    style: const TextStyle(
                                      fontWeight: FontWeight.bold,
                                      fontSize: 14,
                                      color: Colors.black87,
                                    ),
                                  ),
                                  const SizedBox(height: 4),
                                  Text(
                                    _gpsLocked
                                        ? "Locked onto satellites. High-accuracy real-time location tracking active."
                                        : (_lbs 
                                            ? "Estimated from cellular mobile towers. Connecting to satellites..."
                                            : "Searching for satellite signals. Please take the module outdoors for a faster lock."),
                                    style: TextStyle(
                                      fontSize: 12,
                                      color: Colors.grey[600],
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ],
                        ),
                        const Divider(height: 25, thickness: 0.8),
                        // Battery level section
                        Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Icon(
                              _batteryLevel < 20
                                  ? Icons.battery_alert
                                  : (_batteryLevel < 50 ? Icons.battery_3_bar : Icons.battery_full),
                              color: _batteryLevel < 20
                                  ? Colors.red
                                  : (_batteryLevel < 50 ? Colors.orange : Colors.green),
                              size: 24,
                            ),
                            const SizedBox(width: 15),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text(
                                    "Battery Status: $_batteryLevel%",
                                    style: const TextStyle(
                                      fontWeight: FontWeight.bold,
                                      fontSize: 14,
                                      color: Colors.black87,
                                    ),
                                  ),
                                  const SizedBox(height: 4),
                                  Text(
                                    _batteryLevel < 20
                                        ? "Battery level is critical! Please connect the module to a charger immediately."
                                        : (_batteryLevel >= 95 
                                            ? "Battery is fully charged. Ready for long-term tracking."
                                            : "Battery level is healthy. Normal operation mode."),
                                    style: TextStyle(
                                      fontSize: 12,
                                      color: Colors.grey[600],
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ],
                        ),
                        const Divider(height: 25, thickness: 0.8),
                        // GSM Signal Strength Section
                        Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Icon(
                              _csqVal == 0
                                  ? Icons.signal_cellular_null
                                  : Icons.signal_cellular_alt,
                              color: _csqVal == 0
                                  ? Colors.grey
                                  : (_csqVal < 10
                                      ? Colors.red
                                      : (_csqVal < 15
                                          ? Colors.orange
                                          : (_csqVal < 20
                                              ? Colors.amber
                                              : Colors.green))),
                              size: 24,
                            ),
                            const SizedBox(width: 15),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text(
                                    _csqVal == 0
                                        ? "GSM Signal: No Signal"
                                        : (_csqVal < 10
                                            ? "GSM Signal: Very Weak"
                                            : (_csqVal < 15
                                                ? "GSM Signal: Poor Connection"
                                                : (_csqVal < 20
                                                    ? "GSM Signal: Stable Connection"
                                                    : "GSM Signal: Excellent Connection"))),
                                    style: const TextStyle(
                                      fontWeight: FontWeight.bold,
                                      fontSize: 14,
                                      color: Colors.black87,
                                    ),
                                  ),
                                  const SizedBox(height: 4),
                                  Text(
                                    _csqVal == 0
                                        ? "No cellular network connection detected. Ensure your SIM card is active and inserted properly."
                                        : (_csqVal < 10
                                            ? "Signal is critical! Connection may drop. Make sure antenna is securely attached."
                                            : (_csqVal < 15
                                                ? "Poor signal strength. Tracker may consume more battery to maintain connection."
                                                : (_csqVal < 20
                                                    ? "Signal strength is stable. Normal tracking mode active."
                                                    : "Strong connection. Optimal real-time tracking performance."))),
                                    style: TextStyle(
                                      fontSize: 12,
                                      color: Colors.grey[600],
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ],
                        ),
                      ],
                    ),
                  ),

                  // --- PHONE ALARM VOLUME SLIDER CARD ---
                  Container(
                    width: double.infinity,
                    padding: const EdgeInsets.all(15),
                    margin: const EdgeInsets.only(bottom: 15),
                    decoration: BoxDecoration(
                      color: Colors.white,
                      borderRadius: BorderRadius.circular(20),
                      boxShadow: [
                        BoxShadow(
                          color: Colors.black.withOpacity(0.05),
                          blurRadius: 10,
                          offset: const Offset(0, 5),
                        )
                      ],
                    ),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(
                          children: [
                            const Icon(Icons.volume_up, color: Colors.purple, size: 22),
                            const SizedBox(width: 8),
                            Text(
                              "Alarm Volume: ${(_alarmVolume * 100).toInt()}%",
                              style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 14),
                            ),
                          ],
                        ),
                        Slider(
                          value: _alarmVolume,
                          min: 0.0,
                          max: 1.0,
                          activeColor: Colors.purple,
                          inactiveColor: Colors.purple.shade100,
                          onChanged: (val) {
                            setState(() {
                              _alarmVolume = val;
                            });
                            FlutterBackgroundService().invoke('set_volume', {'volume': val});
                          },
                          onChangeEnd: (val) async {
                            final prefs = await SharedPreferences.getInstance();
                            await prefs.setDouble('alarm_volume', val);
                          },
                        ),
                      ],
                    ),
                  ),

                  // --- 6 OPTION CONTROLS (GRID) ---
                  GridView.count(
                    shrinkWrap: true,
                    physics: const NeverScrollableScrollPhysics(),
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

                      _buildGridItem("Phone Alarm", Icons.music_note, Colors.purple, () async {
                        setState(() => _playMusicMode = !_playMusicMode);
                        final prefs = await SharedPreferences.getInstance();
                        await prefs.setBool('play_music', _playMusicMode);
                        FlutterBackgroundService().invoke('set_play_music', {'play_music': _playMusicMode});
                      }, isActive: _playMusicMode),
                      _buildGridItem("GPS Map", Icons.map, Colors.green, () {
                        Navigator.push(context, MaterialPageRoute(builder: (_) => MapScreen(bagLoc: _bag, phoneLoc: _phone, dist: _dist)));
                      }, isActive: true),
                      _buildGridItem("SMS Location", Icons.sms, Colors.teal, () {
                        _sendSmsCommand();
                      }, isActive: true),
                    ],
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
    
    canvas.drawArc(rect, 3.14159, 3.14159, false, bgPaint);
    
    double sweepAngle = (percentage / 100) * 3.14159;
    canvas.drawArc(rect, 3.14159, sweepAngle, false, fgPaint);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => true;
}

// ── MAP SCREEN ───────────────────────────────────────────────────────────────────
class MapScreen extends StatefulWidget {
  final LatLng? bagLoc;
  final LatLng? phoneLoc;
  final double dist;

  const MapScreen({super.key, this.bagLoc, this.phoneLoc, required this.dist});

  @override
  State<MapScreen> createState() => _MapScreenState();
}

class _MapScreenState extends State<MapScreen> {
  List<LatLng> _path = [];
  final DatabaseReference _db = FirebaseDatabase.instance.ref();
  StreamSubscription? _latSub, _lonSub;
  double? _liveLat;
  double? _liveLon;

  @override
  void initState() {
    super.initState();
    _loadHistoryPath();
    _startLiveSub();
  }

  Future<void> _loadHistoryPath() async {
    final prefs = await SharedPreferences.getInstance();
    final history = prefs.getStringList('bag_path_history') ?? [];
    List<LatLng> tempPath = [];
    for (var str in history) {
      final parts = str.split(',');
      if (parts.length == 2) {
        double? lat = double.tryParse(parts[0]);
        double? lon = double.tryParse(parts[1]);
        if (lat != null && lon != null) {
          tempPath.add(LatLng(lat, lon));
        }
      }
    }
    setState(() {
      _path = tempPath;
    });
  }

  void _startLiveSub() {
    _latSub = _db.child("bag_data/lat").onValue.listen((event) {
      final val = event.snapshot.value;
      if (val != null) {
        _liveLat = double.tryParse(val.toString());
        _updateLivePath();
      }
    });

    _lonSub = _db.child("bag_data/lon").onValue.listen((event) {
      final val = event.snapshot.value;
      if (val != null) {
        _liveLon = double.tryParse(val.toString());
        _updateLivePath();
      }
    });
  }

  void _updateLivePath() {
    if (_liveLat != null && _liveLon != null) {
      final newLoc = LatLng(_liveLat!, _liveLon!);
      if (_path.isEmpty || _path.last.latitude != newLoc.latitude || _path.last.longitude != newLoc.longitude) {
        setState(() {
          _path.add(newLoc);
        });
      }
    }
  }

  Future<void> _clearPath() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('bag_path_history');
    setState(() {
      _path.clear();
    });
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text("Path history cleared.")),
      );
    }
  }

  Future<void> _savePath() async {
    if (_path.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text("No path to save!")),
      );
      return;
    }

    final textController = TextEditingController(
      text: "Trip_${DateTime.now().hour}_${DateTime.now().minute}",
    );

    showDialog(
      context: context,
      builder: (context) {
        return AlertDialog(
          title: const Text("Save Current Path"),
          content: TextField(
            controller: textController,
            decoration: const InputDecoration(labelText: "Path Name"),
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text("CANCEL"),
            ),
            TextButton(
              onPressed: () async {
                final String name = textController.text.trim();
                if (name.isEmpty) return;

                final prefs = await SharedPreferences.getInstance();
                List<String> savedNames = prefs.getStringList('saved_paths_list') ?? [];
                
                List<Map<String, double>> pathData = _path
                    .map((pt) => {'lat': pt.latitude, 'lon': pt.longitude})
                    .toList();
                
                savedNames.add(name);
                await prefs.setStringList('saved_paths_list', savedNames);
                
                String serialized = pathData.map((pt) => '${pt['lat']},${pt['lon']}').join(';');
                await prefs.setString('saved_path_$name', serialized);

                if (mounted) {
                  Navigator.pop(context);
                  ScaffoldMessenger.of(context).showSnackBar(
                    SnackBar(content: Text("Path saved as '$name'!")),
                  );
                }
              },
              child: const Text("SAVE"),
            ),
          ],
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    LatLng initialTarget = widget.bagLoc ?? const LatLng(6.128108, 80.561877);

    return Scaffold(
      appBar: AppBar(
        title: const Text("GPS Tracking Map"),
        backgroundColor: const Color(0xFFFFB75E),
        foregroundColor: Colors.white,
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: "Clear Path History",
            onPressed: _clearPath,
          ),
          IconButton(
            icon: const Icon(Icons.save),
            tooltip: "Save Current Path",
            onPressed: _savePath,
          ),
        ],
      ),
      body: widget.bagLoc == null && _path.isEmpty
          ? const Center(child: CircularProgressIndicator())
          : GoogleMap(
              initialCameraPosition: CameraPosition(
                target: _path.isNotEmpty ? _path.last : initialTarget,
                zoom: 17,
              ),
              markers: {
                if (widget.bagLoc != null)
                  Marker(
                    markerId: const MarkerId("bag"),
                    position: widget.bagLoc!,
                    icon: BitmapDescriptor.defaultMarkerWithHue(BitmapDescriptor.hueRed),
                    infoWindow: const InfoWindow(title: "Current Bag Location"),
                  ),
                if (widget.phoneLoc != null)
                  Marker(
                    markerId: const MarkerId("phone"),
                    position: widget.phoneLoc!,
                    icon: BitmapDescriptor.defaultMarkerWithHue(BitmapDescriptor.hueBlue),
                    infoWindow: const InfoWindow(title: "Phone Location"),
                  ),
              },
              polylines: {
                if (_path.isNotEmpty)
                  Polyline(
                    polylineId: const PolylineId("bag_path"),
                    points: _path,
                    color: Colors.blueAccent,
                    width: 4,
                  ),
              },
              myLocationEnabled: true,
              myLocationButtonEnabled: true,
            ),
    );
  }

  @override
  void dispose() {
    _latSub?.cancel();
    _lonSub?.cancel();
    super.dispose();
  }
}
