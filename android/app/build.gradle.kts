plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Version comes from the release tag in CI (android-v1.2.0 -> -PappVersion=1.2.0).
val appVersion = (findProperty("appVersion") as String?) ?: "1.1.0"
val versionParts = appVersion.split(".").map { it.toIntOrNull() ?: 0 } + listOf(0, 0, 0)
val appVersionCode = versionParts[0] * 10000 + versionParts[1] * 100 + versionParts[2]

android {
    namespace = "cloud.jefflab.sleepshuffle"
    compileSdk = 36

    defaultConfig {
        applicationId = "cloud.jefflab.sleepshuffle"
        minSdk = 26
        targetSdk = 36
        versionCode = appVersionCode
        versionName = appVersion
    }

    // Releases are signed with the same key as the copy already on the phone (the Mac's debug
    // keystore), so updates install over it and keep history and bans. CI supplies it from secrets.
    signingConfigs {
        create("release") {
            val ks = System.getenv("SIGNING_KEYSTORE")
            storeFile = if (ks != null) file(ks) else file("${System.getProperty("user.home")}/.android/debug.keystore")
            storePassword = System.getenv("SIGNING_STORE_PASSWORD") ?: "android"
            keyAlias = System.getenv("SIGNING_KEY_ALIAS") ?: "androiddebugkey"
            keyPassword = System.getenv("SIGNING_KEY_PASSWORD") ?: "android"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }

    lint {
        disable += "UnsafeOptInUsageError"
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2025.06.01")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")

    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")

    val media3 = "1.7.1"
    implementation("androidx.media3:media3-exoplayer:$media3")
    implementation("androidx.media3:media3-exoplayer-hls:$media3")
    implementation("androidx.media3:media3-ui:$media3")
    implementation("androidx.media3:media3-session:$media3")
}
