plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

val androidCompileSdk = libs.versions.androidCompileSdk.get().toInt()
val androidMinSdk = libs.versions.androidMinSdk.get().toInt()

android {
    namespace = "com.smlc666.lkmdbg.appdata"
    compileSdk = androidCompileSdk

    defaultConfig {
        minSdk = androidMinSdk
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    testOptions {
        unitTests.isIncludeAndroidResources = true
    }
}

dependencies {
    implementation(project(":shared"))
    implementation(project(":app-domain"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)

    testImplementation(libs.junit4)
    testImplementation(libs.robolectric)
}
