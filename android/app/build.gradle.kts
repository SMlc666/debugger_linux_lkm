import org.gradle.api.GradleException
import org.gradle.api.DefaultTask
import org.gradle.api.file.ConfigurableFileCollection
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.InputFiles
import org.gradle.api.tasks.LocalState
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.gradle.process.ExecOperations
import org.gradle.api.file.FileSystemOperations
import javax.inject.Inject
import java.io.File

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

abstract class BuildBundledAgentTask @Inject constructor(
    private val execOperations: ExecOperations,
    private val fileSystemOperations: FileSystemOperations,
) : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val agentSourceDir: DirectoryProperty

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val sourceFiles: ConfigurableFileCollection

    @get:Input
    abstract val cmakeExecutablePath: org.gradle.api.provider.Property<String>

    @get:Input
    abstract val ninjaExecutablePath: org.gradle.api.provider.Property<String>

    @get:Input
    abstract val toolchainFilePath: org.gradle.api.provider.Property<String>

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:LocalState
    abstract val configureDir: DirectoryProperty

    @TaskAction
    fun buildBundledAgent() {
        val sdk = System.getenv("ANDROID_SDK_ROOT")
            ?: System.getenv("ANDROID_HOME")
            ?: throw GradleException("ANDROID_SDK_ROOT or ANDROID_HOME is required")
        val cmakeBin = File(cmakeExecutablePath.get())
        val ninjaBin = File(ninjaExecutablePath.get())
        val toolchainFile = File(toolchainFilePath.get())
        val configureDirFile = configureDir.get().asFile
        val outputDirFile = outputDir.get().asFile
        val agentSourceDirFile = agentSourceDir.get().asFile

        if (sdk.isBlank())
            throw GradleException("ANDROID_SDK_ROOT or ANDROID_HOME is required")
        if (!cmakeBin.exists())
            throw GradleException("cmake not found at ${cmakeBin.absolutePath}")
        if (!ninjaBin.exists())
            throw GradleException("ninja not found at ${ninjaBin.absolutePath}")
        if (!toolchainFile.exists())
            throw GradleException("toolchain file not found at ${toolchainFile.absolutePath}")

        outputDirFile.mkdirs()

        execOperations.exec {
            commandLine(
                cmakeBin.absolutePath,
                "-S", agentSourceDirFile.absolutePath,
                "-B", configureDirFile.absolutePath,
                "-G", "Ninja",
                "-DANDROID_ABI=arm64-v8a",
                "-DANDROID_PLATFORM=android-26",
                "-DANDROID_STL=c++_shared",
                "-DANDROID_TOOLCHAIN=clang",
                "-DCMAKE_MAKE_PROGRAM=${ninjaBin.absolutePath}",
                "-DCMAKE_TOOLCHAIN_FILE=${toolchainFile.absolutePath}",
            )
        }

        execOperations.exec {
            commandLine(
                cmakeBin.absolutePath,
                "--build", configureDirFile.absolutePath,
                "--parallel",
            )
        }

        fileSystemOperations.copy {
            from(configureDir.file("lkmdbg-agent"))
            into(outputDirFile)
        }
    }
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
    cmakeExecutablePath.set(sdkRoot.map { "$it/cmake/3.22.1/bin/cmake" })
    ninjaExecutablePath.set(sdkRoot.map { "$it/cmake/3.22.1/bin/ninja" })
    toolchainFilePath.set(
        sdkRoot.map { "$it/ndk/$androidNdkVersion/build/cmake/android.toolchain.cmake" },
    )
    outputDir.set(bundledAgentAssetRoot.map { it.dir("agent/arm64-v8a") })
    configureDir.set(layout.buildDirectory.dir("intermediates/bundledAgent/debug/arm64-v8a"))
    description = "Builds the bundled Android root agent and stages it into debug assets."
    group = "build"
}

tasks.matching { it.name == "mergeDebugAssets" }.configureEach {
    dependsOn(buildBundledAgentDebug)
}
