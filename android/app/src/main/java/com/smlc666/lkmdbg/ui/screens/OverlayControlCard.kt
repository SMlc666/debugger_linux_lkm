package com.smlc666.lkmdbg.ui.screens

import android.content.Context
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.overlay.LkmdbgOverlayService
import com.smlc666.lkmdbg.overlay.OverlayPermission
import com.smlc666.lkmdbg.ui.components.PanelCard

@Composable
internal fun OverlayControlCard() {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    var overlayStateVersion by remember { mutableIntStateOf(0) }

    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME)
                overlayStateVersion++
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    val overlayPermissionGranted = remember(overlayStateVersion) {
        OverlayPermission.hasPermission(context)
    }
    val overlayRunning = remember(overlayStateVersion) {
        LkmdbgOverlayService.isRunning()
    }

    PanelCard(
        title = stringResource(R.string.overlay_panel_title),
        subtitle = stringResource(R.string.overlay_panel_subtitle),
        titleIconRes = R.drawable.ic_lkmdbg_radar,
    ) {
        Text(
            text = stringResource(
                R.string.overlay_permission_status,
                localizedBoolean(context, overlayPermissionGranted),
                localizedBoolean(context, overlayRunning),
            ),
        )
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            FilledTonalButton(onClick = {
                OverlayPermission.openSettings(context)
                overlayStateVersion++
            }) {
                Text(stringResource(R.string.overlay_action_grant))
            }
            Button(onClick = {
                LkmdbgOverlayService.start(context)
                overlayStateVersion++
            }, enabled = overlayPermissionGranted && !overlayRunning) {
                Text(stringResource(R.string.overlay_action_show))
            }
            FilledTonalButton(onClick = {
                LkmdbgOverlayService.stop(context)
                overlayStateVersion++
            }, enabled = overlayRunning) {
                Text(stringResource(R.string.overlay_action_hide))
            }
        }
    }
}

private fun localizedBoolean(context: Context, value: Boolean): String =
    if (value) context.getString(R.string.bool_yes) else context.getString(R.string.bool_no)
