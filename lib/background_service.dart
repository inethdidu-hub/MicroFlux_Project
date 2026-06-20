import 'dart:async';
import 'dart:ui';
import 'package:flutter/material.dart';
import 'package:flutter_background_service/flutter_background_service.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_database/firebase_database.dart';
import 'package:geolocator/geolocator.dart';
import 'package:audioplayers/audioplayers.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:vibration/vibration.dart';

Future<void> initializeBackgroundService() async {
  final service = FlutterBackgroundService();

  const AndroidNotificationChannel channel = AndroidNotificationChannel(
    'my_foreground', 
    'Microflux Service', 
    description: 'Running Anti-Theft calculations in background',
    importance: Importance.low, 
  );

  final FlutterLocalNotificationsPlugin flutterLocalNotificationsPlugin =
      FlutterLocalNotificationsPlugin();

  await flutterLocalNotificationsPlugin
      .resolvePlatformSpecificImplementation<
          AndroidFlutterLocalNotificationsPlugin>()
      ?.createNotificationChannel(channel);

  await service.configure(
    androidConfiguration: AndroidConfiguration(
      onStart: onStart,
      autoStart: true,
      isForegroundMode: true,
      notificationChannelId: 'my_foreground',
      initialNotificationTitle: 'Microflux Active',
      initialNotificationContent: 'Monitoring bag location...',
      foregroundServiceNotificationId: 888,
      foregroundServiceTypes: [AndroidForegroundType.location],
    ),
    iosConfiguration: IosConfiguration(
      autoStart: true,
      onForeground: onStart,
    ),
  );
  
  service.startService();
}

@pragma('vm:entry-point')
void onStart(ServiceInstance service) async {
  DartPluginRegistrant.ensureInitialized();
  await Firebase.initializeApp();

  final FlutterLocalNotificationsPlugin flutterLocalNotificationsPlugin =
      FlutterLocalNotificationsPlugin();

  const AndroidInitializationSettings initializationSettingsAndroid =
      AndroidInitializationSettings('@mipmap/ic_launcher');
  const InitializationSettings initializationSettings = InitializationSettings(
    android: initializationSettingsAndroid,
  );

  bool alarmRinging = false;
  double distance = 0;
  double threshold = 20.0;
  bool isReedSwitchOpen = false;
  bool playMusicAlarm = true; 
  double phoneLat = 0;
  double phoneLon = 0;
  double bagLat = 0;
  double bagLon = 0;
  double alarmVolume = 1.0;
  bool isMuted = false;
  bool lastInDanger = false;
  double lastFirebaseDist = -1;

  final db = FirebaseDatabase.instance.ref();
  final _audioPlayer = AudioPlayer();

  _audioPlayer.setAudioContext(AudioContext(
    android: const AudioContextAndroid(
      usageType: AndroidUsageType.alarm,
      contentType: AndroidContentType.music,
      audioFocus: AndroidAudioFocus.gainTransient,
    ),
    iOS: AudioContextIOS(
      category: AVAudioSessionCategory.playback,
      options: const {AVAudioSessionOptions.mixWithOthers},
    ),
  ));
  _audioPlayer.setReleaseMode(ReleaseMode.loop);

  // Load initial settings
  try {
    final prefs = await SharedPreferences.getInstance();
    alarmVolume = prefs.getDouble('alarm_volume') ?? 1.0;
    threshold = prefs.getDouble('threshold') ?? 20.0;
    playMusicAlarm = prefs.getBool('play_music') ?? true;
    _audioPlayer.setVolume(alarmVolume);
  } catch (e) {
    // ignore
  }

  await flutterLocalNotificationsPlugin.initialize(
    settings: initializationSettings,
    onDidReceiveNotificationResponse: (NotificationResponse details) {
      if (details.actionId == 'mute_action') {
        isMuted = true;
        if (alarmRinging) {
          alarmRinging = false;
          service.invoke('ringing_status', {'ringing': false});
          _audioPlayer.stop();
          Vibration.cancel();
          flutterLocalNotificationsPlugin.cancel(id: 999);
        }
        db.child("commands").update({
          "alarm": false,
          "msg": "🛡️ System Secure (Muted)",
        });
      }
    },
  );

  void triggerAlarm(String title, String body) async {
    if (!alarmRinging && playMusicAlarm) {
      alarmRinging = true;
      service.invoke('ringing_status', {'ringing': true});
      _audioPlayer.setVolume(alarmVolume);
      _audioPlayer.play(AssetSource('alarm.ogg'));
      
      bool? hasVibrator = await Vibration.hasVibrator();
      if (hasVibrator == true) {
        Vibration.vibrate(pattern: [500, 1000], repeat: 0);
      }
      
      flutterLocalNotificationsPlugin.show(
        id: 999,
        title: title,
        body: body,
        notificationDetails: const NotificationDetails(
          android: AndroidNotificationDetails(
            'alert_channel',
            'Security Alerts',
            playSound: false,
            importance: Importance.max,
            priority: Priority.high,
            actions: <AndroidNotificationAction>[
              AndroidNotificationAction(
                'mute_action',
                'Mute Alarm',
                showsUserInterface: true,
                cancelNotification: true,
              ),
            ],
          ),
        ),
      );
    }
  }

  void stopAlarm() {
    if (alarmRinging) {
      alarmRinging = false;
      service.invoke('ringing_status', {'ringing': false});
      _audioPlayer.stop();
      Vibration.cancel();
      flutterLocalNotificationsPlugin.cancel(id: 999);
    }
  }

  void checkStatus() {
    bool inDanger = false;
    String cause = "";

    if (isReedSwitchOpen) {
      inDanger = true;
      cause = "Your device has been removed from your bag";
    } else if (distance > threshold) {
      inDanger = true;
      cause = "🚨 BAG OUT OF RANGE! (${distance.toInt()}m)";
    }

    // Update Firebase commands node
    if (inDanger != lastInDanger || (distance - lastFirebaseDist).abs() > 5) {
      lastInDanger = inDanger;
      lastFirebaseDist = distance;
      db.child("commands").update({
        "alarm": inDanger && !isMuted,
        "msg": inDanger 
            ? (isReedSwitchOpen ? "⚠️ DEVICE REMOVED" : "⚠️ PROXIMITY VIOLATION") 
            : "🛡️ System Secure",
        "dist": distance.toStringAsFixed(1),
      });
    }

    if (!inDanger) {
      isMuted = false;
      stopAlarm();
    } else {
      if (playMusicAlarm && !isMuted) {
        triggerAlarm(isReedSwitchOpen ? "SECURITY ALERT" : "MICROFLUX ALERT", cause);
      } else {
        stopAlarm();
      }
    }
  }

  void savePointToHistory(double lat, double lon) async {
    if (lat == 0 || lon == 0) return;
    try {
      final prefs = await SharedPreferences.getInstance();
      List<String> history = prefs.getStringList('bag_path_history') ?? [];
      
      if (history.isNotEmpty) {
        final last = history.last.split(',');
        double lastLat = double.parse(last[0]);
        double lastLon = double.parse(last[1]);
        if ((lastLat - lat).abs() < 0.0001 && (lastLon - lon).abs() < 0.0001) {
          return;
        }
      }
      history.add('$lat,$lon');
      await prefs.setStringList('bag_path_history', history);
    } catch (e) {
      // ignore
    }
  }

  void _updateBgDistance() {
    if (phoneLat != 0 && phoneLon != 0 && bagLat != 0 && bagLon != 0) {
      distance = Geolocator.distanceBetween(phoneLat, phoneLon, bagLat, bagLon);
      checkStatus();
    }
  }

  // 1. Listen to Threshold & Music Settings via events
  service.on('set_play_music').listen((event) {
    if (event != null) {
      playMusicAlarm = event['play_music'] == true;
      if (playMusicAlarm) {
        isMuted = false; // Reset mute when user explicitly turns on Phone Alarm
      }
      checkStatus();
    }
  });

  service.on('set_threshold').listen((event) {
    if (event != null) {
      threshold = double.tryParse(event['threshold'].toString()) ?? 20.0;
      checkStatus();
    }
  });

  // Listen for set_volume / mute_alarm from UI
  service.on('set_volume').listen((event) {
    if (event != null) {
      alarmVolume = double.tryParse(event['volume'].toString()) ?? 1.0;
      _audioPlayer.setVolume(alarmVolume);
    }
  });

  service.on('mute_alarm').listen((event) {
    isMuted = true;
    stopAlarm();
    db.child("commands").update({
      "alarm": false,
      "msg": "🛡️ System Secure (Muted)",
    });
  });

  // Watchdog timer (no SharedPreferences disk I/O, purely memory check)
  Timer.periodic(const Duration(seconds: 5), (timer) {
    checkStatus();
  });

  // 2. Listen to Tamper (Reed Switch)
  db.child("bag_data/tamper").onValue.listen((event) {
    final val = event.snapshot.value;
    isReedSwitchOpen = (val == true || val == "true" || val == 1);
    checkStatus();
  });

  // 3. Listen to Bag Location
  db.child("bag_data/lat").onValue.listen((event) {
    final val = event.snapshot.value;
    if (val != null) {
      bagLat = double.tryParse(val.toString()) ?? 0;
      savePointToHistory(bagLat, bagLon);
      _updateBgDistance();
    }
  });

  db.child("bag_data/lon").onValue.listen((event) {
    final val = event.snapshot.value;
    if (val != null) {
      bagLon = double.tryParse(val.toString()) ?? 0;
      savePointToHistory(bagLat, bagLon);
      _updateBgDistance();
    }
  });

  // 4. Listen to Battery
  int lastNotifiedBattery = 100;
  db.child("bag_data/battery").onValue.listen((event) {
    final val = event.snapshot.value;
    if (val != null) {
      int currentBat = int.tryParse(val.toString()) ?? 0;
      if ((currentBat <= 20 && lastNotifiedBattery > 20) ||
          (currentBat <= 10 && lastNotifiedBattery > 10) ||
          (currentBat <= 5 && lastNotifiedBattery > 5) ||
          (currentBat <= 3 && lastNotifiedBattery > 3)) {
        
        lastNotifiedBattery = currentBat;
        flutterLocalNotificationsPlugin.show(
          id: 888,
          title: "Battery Low",
          body: "Bag battery is at $currentBat%",
          notificationDetails: const NotificationDetails(
            android: AndroidNotificationDetails(
              'battery_channel',
              'Battery Alerts',
            ),
          ),
        );
      } else if (currentBat > lastNotifiedBattery) {
        lastNotifiedBattery = currentBat;
      }
    }
  });

  // 5. Listen to Phone Location stream continuously
  try {
    Geolocator.getPositionStream(
      locationSettings: const LocationSettings(
        accuracy: LocationAccuracy.high,
        distanceFilter: 2,
      ),
    ).listen((Position p) {
      phoneLat = p.latitude;
      phoneLon = p.longitude;
      _updateBgDistance();
    }, onError: (e) {
      // ignore
    });
  } catch (e) {
    // ignore
  }
}
