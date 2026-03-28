package com.smlc666.lkmdbg

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.smlc666.lkmdbg.ui.LkmdbgApp
import com.smlc666.lkmdbg.ui.theme.LkmdbgTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val repository = (application as LkmdbgApplication).sessionRepository
        setContent {
            LkmdbgTheme {
                LkmdbgApp(repository)
            }
        }
    }
}
