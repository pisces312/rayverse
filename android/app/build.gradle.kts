plugins {
    id("com.android.application")
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

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            ndk { abiFilters += listOf("arm64-v8a") }
            signingConfig = signingConfigs.findByName("release")
        }
    }

    signingConfigs {
        create("release") {
            val ks = System.getenv("KEY_STORE")
            val ksPwd = System.getenv("KEY_STORE_PASSWORD")
            val alias = System.getenv("KEY_ALIAS")
            val keyPwd = System.getenv("KEY_PASSWORD")
            if (ks != null && ksPwd != null && alias != null && keyPwd != null) {
                storeFile = file(ks)
                storePassword = ksPwd
                keyAlias = alias
                keyPassword = keyPwd
            }
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
