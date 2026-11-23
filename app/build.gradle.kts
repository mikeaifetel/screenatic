import java.io.File
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.screenatic.receiver"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        applicationId = "com.screenatic.receiver"
        minSdk = libs.versions.minSdk.get().toInt()
        targetSdk = libs.versions.targetSdk.get().toInt()
        versionCode = 3
        versionName = "1.0.2"
        ndk { abiFilters += listOf("armeabi-v7a", "arm64-v8a") }
        // Languages we translate; keeps library translations (Media3 etc.) from claiming ~80 more.
        androidResources.localeFilters += listOf("en", "zh-rCN", "zh-rTW", "ja", "ko", "es", "pt-rBR", "fr", "de", "it")
        // Cast-time rules / A/B / min-version config (docs/ in this repo, served by Cloudflare).
        // Built-in defaults (free, no ads) apply until it's reachable.
        buildConfigField("String", "RULES_URL", "\"https://screenatic.com/rules.json\"")
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

    // Upload key for Google Play / Amazon: kept outside the repo (default ~/.screenatic/keystore.properties,
    // override with SCREENATIC_KEYSTORE_PROPERTIES). Without it, release builds come out unsigned.
    val keystoreProps = File(
        System.getenv("SCREENATIC_KEYSTORE_PROPERTIES") ?: "${System.getProperty("user.home")}/.screenatic/keystore.properties"
    ).takeIf { it.isFile }?.let { f -> Properties().apply { f.inputStream().use { stream -> load(stream) } } }
    signingConfigs {
        if (keystoreProps != null) create("upload") {
            storeFile = file(keystoreProps.getProperty("storeFile"))
            storePassword = keystoreProps.getProperty("storePassword")
            keyAlias = keystoreProps.getProperty("keyAlias")
            keyPassword = keystoreProps.getProperty("keyPassword")
        }
    }

    buildTypes {
        debug {
            // Installs next to a store build; shown as "Screenatic Dev" (src/debug/res).
            applicationIdSuffix = ".dev"
            versionNameSuffix = "-dev"
            // Separate file so rules / min-version changes can be tried on dev builds first.
            buildConfigField("String", "RULES_URL", "\"https://screenatic.com/rules-dev.json\"")
        }
        release {
            signingConfigs.findByName("upload")?.let { signingConfig = it }
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
