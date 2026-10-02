plugins {
    id("com.android.application")
}

// Release signing material lives only in the environment - never in this repo,
// and never behind a default value: a release build without it must fail.
val signingEnv = mapOf(
    "KEY_STORE" to System.getenv("KEY_STORE"),
    "KEY_STORE_PASSWORD" to System.getenv("KEY_STORE_PASSWORD"),
    "KEY_ALIAS" to System.getenv("KEY_ALIAS"),
    "KEY_PASSWORD" to System.getenv("KEY_PASSWORD"),
)
val missingSigningEnv = signingEnv.filterValues { it.isNullOrEmpty() }.keys
if (missingSigningEnv.isNotEmpty() &&
    gradle.startParameter.taskNames.any { it.contains("Release", ignoreCase = true) }) {
    throw GradleException(
        "Release builds read the keystore from the environment. Missing: " +
            missingSigningEnv.joinToString() + ". Use assembleDebug if no signing is needed."
    )
}

android {
    namespace = "com.rayverse.rayman"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.rayverse.rayman"
        minSdk = 21
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
        ndk { abiFilters += listOf("arm64-v8a") }
    }

    // Declared before buildTypes on purpose: the DSL runs in order, so a
    // signingConfigs lookup from buildTypes would otherwise see null.
    signingConfigs {
        create("release") {
            // An explicit release task already failed the check above; this only keeps
            // debug-only builds working while other release tasks still fail in packageRelease.
            if (missingSigningEnv.isEmpty()) {
                storeFile = file(signingEnv.getValue("KEY_STORE"))
                storePassword = signingEnv.getValue("KEY_STORE_PASSWORD")
                keyAlias = signingEnv.getValue("KEY_ALIAS")
                keyPassword = signingEnv.getValue("KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            ndk { abiFilters += listOf("arm64-v8a") }
            signingConfig = signingConfigs.getByName("release")
        }
    }

    externalNativeBuild {
        ndkBuild {
            path = file("src/main/cpp/Android.mk")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.documentfile:documentfile:1.1.0")
}
