import org.gradle.api.GradleException

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

val bundledAgentAssetRoot = layout.buildDirectory.dir("generated/assets/bundledAgent/main")

android {
    namespace = "com.smlc666.lkmdbg"
    compileSdk = 35
    ndkVersion = "27.1.12297006"

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
        release {
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

    buildFeatures {
        compose = true
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
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.service)
    implementation(libs.androidx.activity.compose)
    implementation(libs.kotlinx.coroutines.android)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.compose.ui.tooling.preview)
    debugImplementation(libs.androidx.compose.ui.tooling)
}

val buildBundledAgentDebug by tasks.registering {
    val sdkRoot = providers.environmentVariable("ANDROID_SDK_ROOT")
        .orElse(providers.environmentVariable("ANDROID_HOME"))
    val assetDir = bundledAgentAssetRoot.map { it.dir("agent/arm64-v8a") }
    val buildDir = layout.buildDirectory.dir("intermediates/bundledAgent/debug/arm64-v8a")

    inputs.files(
        fileTree("${projectDir.parentFile}/agent/src/main/cpp"),
        file("${projectDir.parentFile}/agent/CMakeLists.txt"),
        file("${projectDir.parentFile}/../include/lkmdbg_ioctl.h"),
    )
    outputs.dir(assetDir)
    notCompatibleWithConfigurationCache("Builds the bundled Android agent via project.exec and staged asset copying")

    doLast {
        val sdk = sdkRoot.orNull ?: throw GradleException("ANDROID_SDK_ROOT or ANDROID_HOME is required")
        val cmakeBin = file("$sdk/cmake/3.22.1/bin/cmake")
        val ninjaBin = file("$sdk/cmake/3.22.1/bin/ninja")
        val ndkDir = file("$sdk/ndk/${android.ndkVersion}")
        val configureDir = buildDir.get().asFile
        val outDir = assetDir.get().asFile
        val agentSourceDir = file("${projectDir.parentFile}/agent")

        if (!cmakeBin.exists())
            throw GradleException("cmake not found at ${cmakeBin.absolutePath}")
        if (!ninjaBin.exists())
            throw GradleException("ninja not found at ${ninjaBin.absolutePath}")
        if (!ndkDir.exists())
            throw GradleException("ndk not found at ${ndkDir.absolutePath}")

        outDir.mkdirs()

        exec {
            commandLine(
                cmakeBin.absolutePath,
                "-S", agentSourceDir.absolutePath,
                "-B", configureDir.absolutePath,
                "-G", "Ninja",
                "-DANDROID_ABI=arm64-v8a",
                "-DANDROID_PLATFORM=android-26",
                "-DANDROID_STL=c++_shared",
                "-DANDROID_TOOLCHAIN=clang",
                "-DCMAKE_MAKE_PROGRAM=${ninjaBin.absolutePath}",
                "-DCMAKE_TOOLCHAIN_FILE=${ndkDir.absolutePath}/build/cmake/android.toolchain.cmake",
            )
        }

        exec {
            commandLine(
                cmakeBin.absolutePath,
                "--build", configureDir.absolutePath,
                "--parallel",
            )
        }

        copy {
            from(File(configureDir, "lkmdbg-agent"))
            into(outDir)
        }
    }
}

tasks.matching { it.name == "mergeDebugAssets" }.configureEach {
    dependsOn(buildBundledAgentDebug)
}
