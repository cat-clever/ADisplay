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
        versionCode = 555
        versionName = "0.5.55"

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

    // 签名：统一用仓库里那份**公开的**密钥库 app-tv/fallback-signing.p12。
    //
    // 不接环境变量、不接 Secrets。签名在这里只有一个作用：让同一个应用在设备上
    // 能**覆盖升级** —— Android 要求新旧包签名一致，否则必须先卸载，而卸载会清掉
    // 配置与配对密钥。所以签名必须每版都一样，而且不能依赖只有本仓库才有的东西。
    //
    // 早先这里走过「Secrets 里配了私有密钥就优先用」的三级回退，那套东西的麻烦
    // 大于收益：Secrets 不会复制到 fork，可复用工作流也不会自动继承 Secrets，
    // 两处都只是**静默**失效 —— gradle 不报错，直接退回 debug 签名，而 debug
    // 密钥库是每个 runner 构建时现生成的，等于签名每版都变。结果是发出去的包
    // 一直是 debug 签名，用户装新版照样要先卸载，正好绕回要解决的问题上。
    //
    // 公开密钥当然证明不了发布者身份，但这里本来也不需要它证明：所有构建
    // （含 fork）用同一个包名 com.adisplay.tv，一台设备上只能装一个。
    //
    // 路径按**根项目**算：本文件在 app-tv/app/ 下，密钥库在 app-tv/ 下。
    // 按 projectDir 拼会差一级，找不到文件就静默退回 debug 签名。
    val releaseKeystore = project.rootProject.file("fallback-signing.p12")

    // 打进构建日志。签名配错时 gradle 不会报错（会静默退回 debug），只有用户装
    // 新版失败那一刻才会暴露 —— 让 CI 日志里直接能看到用的是哪把密钥。
    logger.lifecycle("APK 签名密钥库：${releaseKeystore.absolutePath}（存在=${releaseKeystore.exists()}）")

    signingConfigs {
        create("release") {
            storeFile = releaseKeystore
            // 公开口令，刻意写在代码里而不是藏起来 —— 它本来就是仓库的一部分，
            // 装作保密只会让人误以为它能证明身份。
            storePassword = "adisplay-fallback"
            // 别名就是密钥库里的 friendlyName（openssl 导出时用 -name adisplay
            // 写进去的），Java 读 PKCS12 时看到的就是它。
            keyAlias = "adisplay"
            keyPassword = "adisplay-fallback"
            // openssl 生成的 PKCS12。显式写出来：默认值随 JDK 版本变过。
            storeType = "PKCS12"
        }
    }

    buildTypes {
        release {
            // 侧载自用，不需要混淆；出问题时要看得懂堆栈（文档 1.1）。
            isMinifyEnabled = false
            // 固定用上面那把密钥库；找不到文件时 gradle 会直接失败，
            // 而不是悄悄退回 debug 签名 —— 失败比发一个签名会变的包好。
            signingConfig = signingConfigs.getByName("release")
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
