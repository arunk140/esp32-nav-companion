plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.navesp32.companion"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.navesp32.companion"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0-phase1"
    }

    buildTypes {
        getByName("debug") {
            applicationIdSuffix = ".debug"
        }
        getByName("release") {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    buildFeatures {
        viewBinding = true
        // Needed for the vendored OsmAnd AIDL client: its interfaces are
        // .aidl files in our own package namespace (net.osmand.aidlapi.*),
        // not a library dependency.
        aidl = true
    }

    // Pure-logic unit tests call code that logs via android.util.Log; return
    // default values instead of throwing "not mocked".
    testOptions {
        unitTests.isReturnDefaultValues = true
    }

    sourceSets["main"].aidl.srcDir("src/main/java")
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.security:security-crypto:1.1.0-alpha06")
    implementation("com.google.android.gms:play-services-location:21.3.0")

    // CameraX + ML Kit QR scanning
    val camerax = "1.3.4"
    implementation("androidx.camera:camera-core:$camerax")
    implementation("androidx.camera:camera-camera2:$camerax")
    implementation("androidx.camera:camera-lifecycle:$camerax")
    implementation("androidx.camera:camera-view:$camerax")
    implementation("com.google.mlkit:barcode-scanning:17.3.0")

    // Pure-logic unit tests (polyline decode, projection, nav loop, share parse).
    testImplementation("junit:junit:4.13.2")
}
