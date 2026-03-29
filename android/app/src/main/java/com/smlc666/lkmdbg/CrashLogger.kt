package com.smlc666.lkmdbg

import android.content.Context
import android.os.Build
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object CrashLogger {
    private const val CRASH_DIR_NAME = "crash"
    private const val LAST_CRASH_FILE_NAME = "last_crash.txt"
    private val timestampFormat = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss_SSS", Locale.US)

    fun install(context: Context) {
        val appContext = context.applicationContext
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            runCatching { writeCrash(appContext, thread, throwable, "uncaught") }
            previous?.uncaughtException(thread, throwable)
                ?: run { throw throwable }
        }
    }

    fun logHandled(context: Context, tag: String, throwable: Throwable) {
        runCatching {
            writeCrash(context.applicationContext, Thread.currentThread(), throwable, tag)
        }
    }

    private fun writeCrash(
        context: Context,
        thread: Thread,
        throwable: Throwable,
        tag: String,
    ) {
        val crashDir = File(context.filesDir, CRASH_DIR_NAME)
        if (!crashDir.exists())
            crashDir.mkdirs()

        val timestamp = timestampFormat.format(Date())
        val report = buildString {
            appendLine("tag=$tag")
            appendLine("timestamp=$timestamp")
            appendLine("thread=${thread.name}")
            appendLine("pid=${android.os.Process.myPid()}")
            appendLine("device=${Build.MANUFACTURER}/${Build.MODEL}")
            appendLine("fingerprint=${Build.FINGERPRINT}")
            appendLine("android=${Build.VERSION.RELEASE} sdk=${Build.VERSION.SDK_INT}")
            appendLine()
            appendLine(stackTraceString(throwable))
        }

        File(crashDir, "crash-$timestamp.txt").writeText(report)
        File(crashDir, LAST_CRASH_FILE_NAME).writeText(report)
    }

    private fun stackTraceString(throwable: Throwable): String {
        val writer = StringWriter()
        PrintWriter(writer).use { printWriter ->
            throwable.printStackTrace(printWriter)
        }
        return writer.toString()
    }
}
