package com.smlc666.lkmdbg.data

import android.content.Context
import android.content.pm.ApplicationInfo
import com.smlc666.lkmdbg.shared.BridgeProcessRecord

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

class AndroidProcessResolver(
    context: Context,
) {
    private val packageManager = context.packageManager
    private val appInfoCache = mutableMapOf<String, ApplicationInfo?>()
    private val labelCache = mutableMapOf<String, String>()

    fun resolve(records: List<BridgeProcessRecord>): List<ResolvedProcessRecord> =
        records.map(::resolveRecord)

    private fun resolveRecord(record: BridgeProcessRecord): ResolvedProcessRecord {
        val processName = record.cmdline.ifBlank { record.comm }.trim().ifBlank { record.comm }
        val packageName = resolveBasePackage(processName)
        val appInfo = packageName?.let(::findApplicationInfo)
        val isAndroidApp = appInfo != null
        val isSystemApp = appInfo != null && (
            (appInfo.flags and ApplicationInfo.FLAG_SYSTEM) != 0 ||
                (appInfo.flags and ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) != 0
            )
        val displayName = appInfo?.let { info ->
            labelCache.getOrPut(info.packageName) {
                packageManager.getApplicationLabel(info).toString()
            }
        } ?: record.comm.ifBlank { processName }

        return ResolvedProcessRecord(
            pid = record.pid,
            uid = record.uid,
            comm = record.comm,
            cmdline = record.cmdline,
            processName = processName,
            displayName = displayName,
            packageName = appInfo?.packageName,
            iconPackageName = appInfo?.packageName,
            isAndroidApp = isAndroidApp,
            isSystemApp = isSystemApp,
        )
    }

    private fun resolveBasePackage(processName: String): String? {
        val token = processName.substringBefore(' ').trim()
        if (token.isEmpty() || token.contains('/'))
            return null

        val candidate = token.substringBefore(':')
        val parts = candidate.split('.')
        if (parts.size < 2 || parts.any { it.isBlank() })
            return null

        return candidate
    }

    private fun findApplicationInfo(packageName: String): ApplicationInfo? =
        appInfoCache.getOrPut(packageName) {
            runCatching {
                @Suppress("DEPRECATION")
                packageManager.getApplicationInfo(packageName, 0)
            }.getOrNull()
        }
}
