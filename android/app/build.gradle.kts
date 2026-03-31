import com.smlc666.gradle.BuildBundledAgentTask

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

val bundledAgentAssetRoot = layout.buildDirectory.dir("generated/assets/bundledAgent/main")
val androidNdkVersion = "27.1.12297006"

android {
    namespace = "com.smlc666.lkmdbg"
    compileSdk = 35
    ndkVersion = androidNdkVersion

    signingConfigs {
        create("fixedDebug") {
            storeFile = file("signing/lkmdbg-debug.p12")
            storePassword = "androiddebug"
            keyAlias = "lkmdbgdebug"
            keyPassword = "androiddebug"
            storeType = "PKCS12"
        }
    }

    defaultConfig {
        applicationId = "com.smlc666.lkmdbg"
        minSdk = 26
        targetSdk = 35
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

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("fixedDebug")
        }

        release {
            signingConfig = signingConfigs.getByName("fixedDebug")
            isMinifyEnabled = false
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
            version = "3.22.1"
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

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.service)
    implementation(libs.kotlinx.coroutines.android)

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
    outputDir.set(layout.buildDirectory.dir("generated/assets/bundledAgent/main/agent/arm64-v8a"))
    configureDir.set(layout.buildDirectory.dir("intermediates/bundledAgent/debug/arm64-v8a"))
    description = "Builds the bundled Android root agent and stages it into debug assets."
    group = "build"
}

tasks.matching { it.name == "mergeDebugAssets" }.configureEach {
    dependsOn(buildBundledAgentDebug)
}
