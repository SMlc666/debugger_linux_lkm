package com.smlc666.lkmdbg.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.ui.components.PanelCard
import com.smlc666.lkmdbg.ui.screens.OverlayControlCard

@Composable
internal fun LauncherScreen() {
    val context = LocalContext.current

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(
                brush = Brush.verticalGradient(
                    colors = listOf(
                        Color(0xFF07111A),
                        Color(0xFF0A1620),
                        Color(0xFF0E1E25),
                    ),
                ),
            )
            .padding(horizontal = 20.dp, vertical = 28.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .widthIn(max = 620.dp),
        ) {
            LauncherIntroCard()
            Spacer(Modifier.height(18.dp))
            OverlayControlCard()
        }
    }
}

@Composable
private fun LauncherIntroCard() {
    val context = LocalContext.current

    PanelCard(
        title = context.getString(R.string.launcher_panel_title),
        subtitle = context.getString(R.string.launcher_panel_subtitle),
        titleIconRes = R.drawable.ic_lkmdbg_terminal,
    ) {
        Text(
            text = context.getString(R.string.launcher_panel_body),
            style = MaterialTheme.typography.bodyLarge,
        )
    }
}
