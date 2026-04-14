package com.smlc666.lkmdbg.data

enum class ProcessFilter {
    All,
    AndroidApps,
    CommandLine,
    SystemApps,
    UserApps,
    ;

    fun matches(process: ResolvedProcessRecord): Boolean =
        when (this) {
            All -> true
            AndroidApps -> process.isAndroidApp
            CommandLine -> !process.isAndroidApp
            SystemApps -> process.isAndroidApp && process.isSystemApp
            UserApps -> process.isAndroidApp && !process.isSystemApp
        }
}

data class ResolvedProcessRecord(
    val pid: Int,
    val uid: Int,
    val comm: String,
    val cmdline: String,
    val processName: String,
    val displayName: String,
    val packageName: String?,
    val iconPackageName: String?,
    val isAndroidApp: Boolean,
    val isSystemApp: Boolean,
)
