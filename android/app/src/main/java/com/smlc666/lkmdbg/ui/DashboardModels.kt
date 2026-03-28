package com.smlc666.lkmdbg.ui

import android.content.Context
import com.smlc666.lkmdbg.R

internal data class DashboardMetric(
    val label: String,
    val value: String,
)

internal data class DashboardState(
    val chips: List<String>,
    val scanSummary: String,
    val memoryRows: List<DashboardMetric>,
)

internal fun sampleDashboardState(context: Context): DashboardState =
    DashboardState(
        chips = listOf(
            context.getString(R.string.dashboard_chip_root),
            context.getString(R.string.dashboard_chip_pipe),
            context.getString(R.string.dashboard_chip_md3),
            context.getString(R.string.dashboard_chip_gki),
        ),
        scanSummary = context.getString(R.string.memory_scan_summary),
        memoryRows = listOf(
            DashboardMetric(context.getString(R.string.memory_metric_watchlist), "4"),
            DashboardMetric(context.getString(R.string.memory_metric_last_read), "128 KiB in 3.1 ms"),
            DashboardMetric(context.getString(R.string.memory_metric_last_write), "32 KiB in 0.9 ms"),
        ),
    )
