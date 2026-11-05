plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// A branch build can be installed beside the main app: `./gradlew assembleDebug -PappIdSuffix=.engine2`
// builds dev.nyanroid.engine2, named "Nyanroid engine2", with saves and settings of its own.
val appIdSuffix = providers.gradleProperty("appIdSuffix").orNull.orEmpty().trim().let {
    if (it.isEmpty() || it.startsWith(".")) it else ".$it"
}

android {
    namespace = "dev.nyanroid"
    compileSdk = 35
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "dev.nyanroid$appIdSuffix"
        if (appIdSuffix.isNotEmpty()) resValue("string", "app_name", "Nyanroid ${appIdSuffix.removePrefix(".")}")
        minSdk = 27  // Android 8.1, the first with a dependable AAudio; newer calls check the version
        targetSdk = 35
        versionCode = 4
        versionName = "0.3.1"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    buildTypes {
        debug {
            // Native code is far too slow unoptimised for image decoding.
            externalNativeBuild { cmake { arguments += listOf("-DCMAKE_BUILD_TYPE=RelWithDebInfo") } }
        }
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    // Fonts in the project's fonts/ folder (not in git; see fonts/README.md) go into debug builds
    // only: release builds, the ones to publish, never carry them (their licences rarely allow it).
    sourceSets {
        getByName("debug") {
            assets.srcDir(file("../../fonts"))
        }
    }
    androidResources {
        noCompress += listOf("ttf", "ttc", "otf", "TTF", "TTC", "OTF")
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

kotlin {
    jvmToolchain(17)
}

// The licences go into every APK (the library's About screen shows them): the project's GPL text and
// the third-party notices, from the repository's root.
abstract class LicenseAssets : DefaultTask() {
    @get:InputFiles abstract val sources: ConfigurableFileCollection
    @get:OutputDirectory abstract val outputDir: DirectoryProperty

    @TaskAction
    fun copy() {
        val out = outputDir.get().asFile
        out.deleteRecursively()
        out.mkdirs()
        sources.files.forEach { it.copyTo(File(out, it.name), overwrite = true) }
    }
}

val licenseAssets = tasks.register<LicenseAssets>("licenseAssets") {
    sources.from(rootProject.file("../LICENSE"), rootProject.file("../THIRD_PARTY_NOTICES.txt"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(licenseAssets, LicenseAssets::outputDir)
    }
}
