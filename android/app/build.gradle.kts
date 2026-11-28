plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("com.chaquo.python")
}

val generatedWebAssets = layout.buildDirectory.dir("generated/trackport-assets")
val sourceWebAssetsCandidates = listOf(
    file("../../artifacts/trackport/native/app"),
    file("../../source/app"),
)
val sourceWebAssets =
    sourceWebAssetsCandidates.firstOrNull { it.isDirectory } ?: sourceWebAssetsCandidates.first()

android {
    namespace = "com.tech953.trackport"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.tech953.trackport"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0-test"
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    sourceSets.getByName("main").assets.srcDir(generatedWebAssets)

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

chaquopy {
    defaultConfig {
        version = "3.11"
        pip {
            install("requests==2.34.2")
            install("PySocks==1.7.1")
            install("yt-dlp==2026.11.23")
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
}

val prepareTrackportWebAssets = tasks.register<Exec>("prepareTrackportWebAssets") {
    inputs.dir(sourceWebAssets)
    inputs.dir(file("src/main/assets/overlay"))
    outputs.dir(generatedWebAssets)

    commandLine(
        "python3",
        file("../scripts/prepare_web.py").absolutePath,
        "--source",
        sourceWebAssets.absolutePath,
        "--output",
        generatedWebAssets.get().dir("www").asFile.absolutePath,
        "--overlay",
        file("src/main/assets/overlay").absolutePath,
    )
}

tasks.named("preBuild").configure {
    dependsOn(prepareTrackportWebAssets)
}
