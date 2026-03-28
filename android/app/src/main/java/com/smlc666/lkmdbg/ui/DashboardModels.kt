package com.smlc666.lkmdbg.ui

import android.content.Context
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.shared.DashboardEvent
import com.smlc666.lkmdbg.shared.DashboardProcess
import com.smlc666.lkmdbg.shared.DashboardThread

internal data class DashboardMetric(
    val label: String,
    val value: String,
)

internal data class DashboardState(
    val chips: List<String>,
    val processes: List<DashboardProcess>,
    val threads: List<DashboardThread>,
    val events: List<DashboardEvent>,
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
        processes = listOf(
            DashboardProcess(4321, "com.example.game", "com.example.game", "arm64", context.getString(R.string.process_state_attached)),
            DashboardProcess(5544, "surfaceflinger", "system", "arm64", context.getString(R.string.process_state_idle)),
            DashboardProcess(6110, "zygote64", "system", "arm64", context.getString(R.string.process_state_available)),
        ),
        threads = listOf(
            DashboardThread(4321, "RenderThread", "0x00000074ab12c090", context.getString(R.string.thread_state_running)),
            DashboardThread(4338, "UnityMain", "0x00000074ab14f5a8", context.getString(R.string.thread_state_stopped)),
            DashboardThread(4349, "Worker-3", "0x00000074ab101c44", context.getString(R.string.thread_state_sleeping)),
        ),
        events = listOf(
            DashboardEvent(
                context.getString(R.string.event_title_syscall_rule),
                context.getString(R.string.event_detail_syscall_rule),
                "info",
                "06:42:11",
            ),
            DashboardEvent(
                context.getString(R.string.event_title_hook_state),
                context.getString(R.string.event_detail_hook_state),
                "ok",
                "06:41:58",
            ),
            DashboardEvent(
                context.getString(R.string.event_title_memory_scan),
                context.getString(R.string.event_detail_memory_scan),
                "info",
                "06:41:20",
            ),
        ),
        scanSummary = context.getString(R.string.memory_scan_summary),
        memoryRows = listOf(
            DashboardMetric(context.getString(R.string.memory_metric_watchlist), "4"),
            DashboardMetric(context.getString(R.string.memory_metric_last_read), "128 KiB in 3.1 ms"),
            DashboardMetric(context.getString(R.string.memory_metric_last_write), "32 KiB in 0.9 ms"),
        ),
    )
