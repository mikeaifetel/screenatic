plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "app.screenhop.receiver"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        applicationId = "app.screenhop.receiver"
        minSdk = libs.versions.minSdk.get().toInt()
        targetSdk = libs.versions.targetSdk.get().toInt()
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += listOf("armeabi-v7a", "arm64-v8a") }
        // Cast-time rules / A/B / min-version config (GitHub Pages, docs/ in this repo).
        // Built-in defaults (free, no ads) apply until it's reachable.
        buildConfigField("String", "RULES_URL", "\"https://mikeaifetel.github.io/screenhop/rules.json\"")
    }

    // Two store channels share one codebase; store SDKs live in src/google and src/amazon.
    flavorDimensions += "store"
    productFlavors {
        create("google") {
            dimension = "store"
            buildConfigField("String", "STORE", "\"google\"")
        }
        create("amazon") {
            dimension = "store"
            buildConfigField("String", "STORE", "\"amazon\"")
        }
    }

    buildTypes {
        debug {
            // Installs next to a store build; shown as "ScreenHop Dev" (src/debug/res).
            applicationIdSuffix = ".dev"
            versionNameSuffix = "-dev"
            // Separate file so rules / min-version changes can be tried on dev builds first.
            buildConfigField("String", "RULES_URL", "\"https://mikeaifetel.github.io/screenhop/rules-dev.json\"")
        }
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin { compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) } }

dependencies {
    implementation(project(":core"))
    implementation(project(":protocol-airplay"))
    implementation(project(":protocol-dlna"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.service)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.tv.material)
    debugImplementation(libs.androidx.compose.ui.tooling)

    implementation(libs.androidx.media3.exoplayer)
    implementation(libs.androidx.media3.exoplayer.hls)
    implementation(libs.androidx.media3.ui)

    // Ad SDKs are added per flavor once the GPL/proprietary-SDK question is settled (see README):
    // "googleImplementation"(...)  e.g. IMA SDK
    // "amazonImplementation"(...)  e.g. APS
}
