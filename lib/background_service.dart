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
      
  final db = FirebaseDatabase.instance.ref();
  
  bool alarmRinging = false;
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
  double distance = 0;
  double threshold = 20.0;
  bool isReedSwitchOpen = false;
  bool playMusicAlarm = true; // Read from settings later
  double phoneLat = 0;
  double phoneLon = 0;
  double bagLat = 0;
  double bagLon = 0;

  void triggerAlarm(String title, String body) async {
    if (!alarmRinging && playMusicAlarm) {
      alarmRinging = true;
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
            playSound: false, // Tone is played by RingtonePlayer
          ),
        ),
      );
    }
  }

  void stopAlarm() {
    if (alarmRinging) {
      alarmRinging = false;
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
      cause = "🚨 BAG OPENED! Reed switch disconnected!";
    } else if (distance > threshold) {
      inDanger = true;
      cause = "🚨 BAG OUT OF RANGE! (${distance.toInt()}m)";
    }

    if (inDanger) {
      triggerAlarm("MICROFLUX ALERT", cause);
    } else {
      stopAlarm();
    }
  }

  // 1. Listen to Threshold & Music Settings
  Timer.periodic(const Duration(seconds: 3), (timer) async {
    final prefs = await SharedPreferences.getInstance();
    threshold = prefs.getDouble('threshold') ?? 20.0;
    playMusicAlarm = prefs.getBool('play_music') ?? true;
    checkStatus();
  });

  // 2. Listen to Reed Switch
  db.child("bag_data/reed_switch").onValue.listen((event) {
    final val = event.snapshot.value;
    isReedSwitchOpen = (val == true || val == "true" || val == 1);
    checkStatus();
  });

  // 3. Listen to Bag Location
  db.child("bag_data/bag_location").onValue.listen((event) {
    final data = event.snapshot.value;
    if (data == null) return;
    try {
      final d = Map<dynamic, dynamic>.from(data as Map);
      bagLat = double.tryParse(d['lat']?.toString() ?? '0') ?? 0;
      bagLon = double.tryParse(d['lon']?.toString() ?? '0') ?? 0;
      
      if (phoneLat != 0 && phoneLon != 0) {
        distance = Geolocator.distanceBetween(phoneLat, phoneLon, bagLat, bagLon);
        checkStatus();
      }
    } catch (e) {
      // Data parse issue
    }
  });

  // 4. Listen to Battery and remind percentages
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
        lastNotifiedBattery = currentBat; // Reset on charge
      }
    }
  });

  // 5. Listen to Phone Location stream continuously
  Geolocator.getPositionStream(
    locationSettings: const LocationSettings(
      accuracy: LocationAccuracy.high,
      distanceFilter: 2,
    ),
  ).listen((Position p) {
    phoneLat = p.latitude;
    phoneLon = p.longitude;
    if (bagLat != 0 && bagLon != 0) {
      distance = Geolocator.distanceBetween(phoneLat, phoneLon, bagLat, bagLon);
      checkStatus();
    }
  });
}
