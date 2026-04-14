package com.smlc666.lkmdbg.data

import android.content.Context
import android.content.pm.ApplicationInfo
import com.smlc666.lkmdbg.shared.BridgeProcessRecord

class AndroidProcessResolver(
    context: Context,
) {
    private val packageManager = context.packageManager
    private val appInfoCache = mutableMapOf<String, ApplicationInfo?>()
    private val labelCache = mutableMapOf<String, String>()
    private val uidPackagesCache = mutableMapOf<Int, List<String>>()

    fun resolve(records: List<BridgeProcessRecord>): List<ResolvedProcessRecord> =
        records.map(::resolveRecord)

    private fun resolveRecord(record: BridgeProcessRecord): ResolvedProcessRecord {
        val processName = record.cmdline.ifBlank { record.comm }.trim().ifBlank { record.comm }
        val basePackage = resolveBasePackage(processName)
        val uidPackages = resolvePackagesForUid(record.uid)
        val packageName = resolvePackageName(processName, basePackage, uidPackages)
        val appInfo = packageName?.let(::findApplicationInfo)
            ?: uidPackages.firstNotNullOfOrNull(::findApplicationInfo)
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

    private fun resolvePackageName(
        processName: String,
        basePackage: String?,
        uidPackages: List<String>,
    ): String? {
        val processToken = processName.substringBefore(' ').trim()
        if (basePackage != null && uidPackages.contains(basePackage))
            return basePackage
        if (basePackage != null && findApplicationInfo(basePackage) != null)
            return basePackage
        uidPackages.firstOrNull { pkg ->
            processToken == pkg || processToken.startsWith("$pkg:")
        }?.let { return it }
        if (uidPackages.size == 1)
            return uidPackages.first()
        return uidPackages.firstOrNull() ?: basePackage
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

    private fun resolvePackagesForUid(uid: Int): List<String> =
        uidPackagesCache.getOrPut(uid) {
            packageManager.getPackagesForUid(uid)?.toList().orEmpty()
        }
}
