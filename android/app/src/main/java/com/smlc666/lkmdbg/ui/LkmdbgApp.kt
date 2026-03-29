package com.smlc666.lkmdbg.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.smlc666.lkmdbg.data.SessionBridgeRepository

@Composable
fun LkmdbgApp(repository: SessionBridgeRepository) {
    val context = LocalContext.current
    val dashboardState = remember(context) { sampleDashboardState(context) }
    val sessionState by repository.state.collectAsStateWithLifecycle()

    LauncherScreen(
        dashboardState = dashboardState,
        sessionState = sessionState,
    )
}
