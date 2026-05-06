plugins {
    id("com.android.application")
    id("com.google.gms.google-services")
    id("kotlin-android")
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.example.anti_theft_app"
    
    // ✅ UPGRADED: Set to 36 to satisfy 2026 plugin requirements
    compileSdk = 36 
    ndkVersion = flutter.ndkVersion

    compileOptions {
        isCoreLibraryDesugaringEnabled = true
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_17.toString()
    }

    defaultConfig {
        applicationId = "com.example.anti_theft_app"
        
        // Ensure this is at least 21 for modern plugins
        minSdk = flutter.minSdkVersion 
        
        // ✅ UPGRADED: Matches compileSdk to stop the warnings
        targetSdk = 36 
        
        versionCode = flutter.versionCode
        versionName = flutter.versionName
        multiDexEnabled = true
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

dependencies {
    // ✅ KEEP THIS: Required for v21.0.0 notification stability
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.1.4")
}
