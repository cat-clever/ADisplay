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
        versionCode = 545
        versionName = "0.5.45"

        // 只打 arm64-v8a。
        //
        // 这不是「能少打就少打」的优化，而是必须的：核心库只在 CI 里针对
        // arm64-v8a 交叉编译一次，jniLibs 下也只有这一份 .so。abiFilters 里
        // 每多列一个 ABI，就等于承诺这个 ABI 也有自己的 .so —— 列了却没有，
        // Android 装的时候不报错，装上跑起来才在加载库时抛
        // UnsatisfiedLinkError，表现是「一装上就闪退」。
        //
        // 电视盒子（Fire TV、Chromecast、主流国产盒子）都是 arm64。真要覆盖
        // 32 位的老板子，得先在 CI 里补出 armeabi-v7a 的 libcastcore.so，
        // 再把那个 ABI 加回这一行 —— 只改这里是不行的。
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    // 签名：三级优先。
    //
    //   1. 环境变量（CI 里由仓库 Secrets 提供）—— 你自己的私有密钥
    //   2. 仓库里那份**公开的兜底密钥** —— 给 fork 用的
    //   3. debug 签名 —— 兜底中的兜底，正常不会走到
    //
    // 为什么必须给 fork 一份兜底密钥：GitHub 的 Secrets **不会**复制到 fork
    // （fork 也读不到原仓库的），所以别人 fork 之后那几个环境变量是空的。
    // 没有兜底的话，他们的包每次都落到 debug 签名 —— 而 debug 密钥库是每台
    // 机器各自生成的，CI 每次都是全新 runner，于是每个版本签名都不同，装新版
    // 必须先卸载，卸载又会清掉配置与配对密钥。
    //
    // 兜底密钥是公开的（就在 app-tv/fallback-signing.p12，口令写在下面），
    // 所以它证明不了发布者身份。但签名在这里的作用本来就是「同一台设备上能
    // 覆盖升级」，不是身份认证 —— 何况所有 fork 用同一个包名 com.adisplay.tv，
    // 本来也只能装一个。想要私有密钥，把那四个 Secrets 配上即会自动优先。
    val fallbackKeystore = File(project.projectDir, "fallback-signing.p12")
    val secretKeystorePath = System.getenv("ADISPLAY_KEYSTORE_FILE")
    val useSecretKey = !secretKeystorePath.isNullOrEmpty() && File(secretKeystorePath).exists()
    val useFallbackKey = !useSecretKey && fallbackKeystore.exists()

    signingConfigs {
        create("release") {
            if (useSecretKey) {
                storeFile = File(secretKeystorePath)
                storePassword = System.getenv("ADISPLAY_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("ADISPLAY_KEY_ALIAS")
                keyPassword = System.getenv("ADISPLAY_KEY_PASSWORD")
            } else if (useFallbackKey) {
                storeFile = fallbackKeystore
                // 公开的兜底口令，刻意写在代码里而不是藏起来 —— 它本来就是
                // 仓库的一部分，装作保密只会让人误以为它能证明身份。
                storePassword = "adisplay-fallback"
                keyAlias = "adisplay"
                keyPassword = "adisplay-fallback"
            }
            // 两种密钥库都是 openssl 生成的 PKCS12。显式写出来：默认值随 JDK
            // 版本变过，写死更稳。
            storeType = "PKCS12"
        }
    }

    buildTypes {
        release {
            // 侧载自用，不需要混淆；出问题时要看得懂堆栈（文档 1.1）。
            isMinifyEnabled = false
            signingConfig = if (useSecretKey || useFallbackKey) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
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

    // 刻意【不】开 externalNativeBuild。
    //
    // Gradle 自己编 C++ 就得再搭一遍 vcpkg + NDK 工具链，等于把主构建里
    // 已经验证过的那套东西复制成两份维护（app/src/main/cpp/CMakeLists.txt
    // 开头也写了同样的理由）。
    //
    // 实际做法是：CI 用 NDK 编出 libcastcore.so 与 libadisplay_jni.so，
    // 拷到 src/main/jniLibs/arm64-v8a/ 下。src/main/jniLibs 是 Gradle 的
    // 约定目录，默认就会打进 APK 的 lib/<ABI>/，这里不需要任何额外配置 ——
    // 两个 .so 缺一个的话构建照样成功，所以 CI 里额外拆包断言了一次。
    //
    // 注意 native 方法的类名 com.adisplay.tv.AdDisplayNative 必须与包名一致，
    // 否则 JVM 找不到 Java_com_adisplay_tv_ 开头的那些符号。

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

    // 播放器（文档 3.2）：DLNA / AirPlay 视频推送过来的是一条 URL
    // （见 include/adisplay/adisplay.h 的 on_media_url），拉流、解复用、硬解、
    // 音画同步这一整套交给 Media3，比自己在 JNI 旁边再写一个播放器靠谱得多，
    // 电视盒子上支持的格式也最全。
    //
    // 1.4.1 是 compileSdk 34 还能编的最后一档：1.5.0 起 AAR 元数据里写了
    // minCompileSdk 35，配这里的 compileSdk 34 会在 checkDebugAarMetadata
    // 阶段直接失败。将来升 compileSdk 到 35 时这两个版本号可以一起往上走。
    //
    // 纯 Java/Kotlin，不带 .so，所以上面 ndk.abiFilters 那条不用动。
    implementation("androidx.media3:media3-exoplayer:1.4.1")
    implementation("androidx.media3:media3-ui:1.4.1")
}
