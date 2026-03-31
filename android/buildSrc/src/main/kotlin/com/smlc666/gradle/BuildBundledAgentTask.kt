package com.smlc666.gradle

import org.gradle.api.DefaultTask
import org.gradle.api.GradleException
import org.gradle.api.file.ConfigurableFileCollection
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.FileSystemOperations
import org.gradle.api.provider.Property
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.InputFiles
import org.gradle.api.tasks.LocalState
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.gradle.process.ExecOperations
import java.io.File
import javax.inject.Inject

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
    abstract val sdkRootPath: Property<String>

    @get:Input
    abstract val ndkVersion: Property<String>

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:LocalState
    abstract val configureDir: DirectoryProperty

    @TaskAction
    fun buildBundledAgent() {
        val sdkRoot = sdkRootPath.get()
        val cmakeBin = File("$sdkRoot/cmake/3.22.1/bin/cmake")
        val ninjaBin = File("$sdkRoot/cmake/3.22.1/bin/ninja")
        val toolchainFile = File("$sdkRoot/ndk/${ndkVersion.get()}/build/cmake/android.toolchain.cmake")
        val configureDirFile = configureDir.get().asFile
        val outputDirFile = outputDir.get().asFile
        val agentSourceDirFile = agentSourceDir.get().asFile

        if (sdkRoot.isBlank())
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
