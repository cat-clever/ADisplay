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
        // versionCode 按 主*10000 + 次*100 + 修订 计算：
        //   0.4.0 -> 400，0.4.1 -> 401，1.0.0 -> 10000
        // 规则固定下来，升级时不用每次临时决定该加多少。
        // 它必须随版本递增 —— 不变的话 Android 会认为是同一个包，
        // 覆盖安装时不会更新。
        versionCode = 401
        versionName = "0.4.1"

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

    // Compose Compiler 的版本必须与 Kotlin 版本严格配对，否则编译期直接报
    // 「This version (x) of the Compose Compiler requires Kotlin version y」。
    // 1.5.14 对应 Kotlin 1.9.24（见 top-level build.gradle.kts 里的 kotlin 版本）。
    // 将来升 Kotlin 到 2.x 时，这一项要换成 org.jetbrains.kotlin.plugin.compose 插件。
    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
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
