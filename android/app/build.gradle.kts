import com.smlc666.gradle.BuildBundledAgentTask

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.compose.compiler)
}

val bundledAgentAssetRoot = layout.buildDirectory.dir("generated/assets/bundledAgent/main")
val androidCompileSdk = libs.versions.androidCompileSdk.get().toInt()
val androidTargetSdk = libs.versions.androidTargetSdk.get().toInt()
val androidMinSdk = libs.versions.androidMinSdk.get().toInt()
val androidNdkVersion = libs.versions.androidNdk.get()
val androidCmakeVersion = libs.versions.androidCmake.get()

val releaseStoreFilePath = providers.gradleProperty("android.release.storeFile").orNull
val releaseStorePassword = providers.gradleProperty("android.release.storePassword").orNull
val releaseKeyAlias = providers.gradleProperty("android.release.keyAlias").orNull
val releaseKeyPassword = providers.gradleProperty("android.release.keyPassword").orNull
val hasReleaseSigning = listOf(
    releaseStoreFilePath,
    releaseStorePassword,
    releaseKeyAlias,
    releaseKeyPassword,
).all { !it.isNullOrBlank() }

android {
    namespace = "com.smlc666.lkmdbg"
    compileSdk = androidCompileSdk
    ndkVersion = androidNdkVersion

    signingConfigs {
        create("fixedDebug") {
            storeFile = file("signing/lkmdbg-debug.p12")
            storePassword = "androiddebug"
            keyAlias = "lkmdbgdebug"
            keyPassword = "androiddebug"
            storeType = "PKCS12"
        }
        if (hasReleaseSigning) {
            create("release") {
                storeFile = file(releaseStoreFilePath!!)
                storePassword = releaseStorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
                storeType = "PKCS12"
            }
        }
    }

    defaultConfig {
        applicationId = "com.smlc666.lkmdbg"
        minSdk = androidMinSdk
        targetSdk = androidTargetSdk
        versionCode = 1
        versionName = "0.1.0"

        vectorDrawables {
            useSupportLibrary = true
        }

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    buildFeatures {
        compose = true
    }

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("fixedDebug")
        }

        release {
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    testOptions {
        unitTests.isIncludeAndroidResources = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = androidCmakeVersion
        }
    }

    sourceSets {
        getByName("main").assets.srcDir(bundledAgentAssetRoot)
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    implementation(project(":shared"))
    implementation(project(":app-domain"))
    implementation(project(":app-data"))
    implementation(project(":app-ui"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.service)
    implementation(libs.google.material)
    implementation(libs.kotlinx.coroutines.android)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.activity.compose)

    testImplementation(libs.junit4)
    testImplementation(libs.robolectric)
}

val buildBundledAgentDebug by tasks.registering(BuildBundledAgentTask::class) {
    val sdkRoot = providers.environmentVariable("ANDROID_SDK_ROOT")
        .orElse(providers.environmentVariable("ANDROID_HOME"))
    agentSourceDir.set(layout.projectDirectory.dir("../agent"))
    sourceFiles.from(
        fileTree(layout.projectDirectory.dir("../agent/src/main/cpp")),
        layout.projectDirectory.file("../agent/CMakeLists.txt"),
        layout.projectDirectory.file("../../include/lkmdbg_ioctl.h"),
    )
    sdkRootPath.set(sdkRoot)
    ndkVersion.set(androidNdkVersion)
    cmakeVersion.set(androidCmakeVersion)
    outputDir.set(layout.buildDirectory.dir("generated/assets/bundledAgent/main/agent/arm64-v8a"))
    configureDir.set(layout.buildDirectory.dir("intermediates/bundledAgent/debug/arm64-v8a"))
    description = "Builds the bundled Android root agent and stages it into debug assets."
    group = "build"
}

tasks.matching { it.name == "mergeDebugAssets" }.configureEach {
    dependsOn(buildBundledAgentDebug)
}

tasks.matching {
    it.name.startsWith("generateDebugLint") || it.name == "generateDebugLintReportModel"
}.configureEach {
    dependsOn(buildBundledAgentDebug)
}
