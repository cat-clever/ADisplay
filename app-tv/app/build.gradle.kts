plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.adisplay.tv"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.adisplay.tv"
        // 文档 5.1：最低 Android 8.0（API 26）。
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        // 自用项目，全 ABI 打进一个 APK 即可（文档 2.5）。
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
        }
    }

    buildTypes {
        release {
            // 侧载自用，不需要混淆；出问题时要看得懂堆栈（文档 1.1）。
            isMinifyEnabled = false
            // 自用不签名发布，用 debug 签名避免 CI 里管理密钥。
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }

    // 批次 0 尚未接入 C++ 核心（JNI 随批次 5），先不开 externalNativeBuild。
    // 接入时需要：
    //   externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.09.00"))

    // Compose for TV 的 Material 组件，遥控器焦点导航由它负责（文档 4.5）。
    implementation("androidx.tv:tv-material:1.0.0")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")

    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")
}
