package com.smlc666.lkmdbg

import android.app.Application
import com.smlc666.lkmdbg.data.SessionBridgeRepository

class LkmdbgApplication : Application() {
    val sessionRepository: SessionBridgeRepository by lazy {
        SessionBridgeRepository(applicationContext)
    }
}
