package com.tech953.trackport

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.webkit.JavascriptInterface
import android.webkit.JsResult
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.TextView
import android.widget.Toast
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.Executors

class MainActivity : Activity() {
    private lateinit var webView: WebView
    private lateinit var loadingView: TextView
    private val background = Executors.newSingleThreadExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())
    private var serverPort = 0
    private var pendingFileChooser: ValueCallback<Array<Uri>>? = null
    private var pendingExport: PendingExport? = null

    private data class PendingExport(val file: File, val name: String)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        webView = WebView(this)
        loadingView = TextView(this).apply {
            text = "Starting Trackport…"
            textSize = 16f
            setTextColor(android.graphics.Color.rgb(32, 57, 47))
            setBackgroundColor(android.graphics.Color.rgb(246, 245, 240))
            gravity = android.view.Gravity.CENTER
            setPadding(32, 32, 32, 32)
        }
        setContentView(loadingView)

        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = false
            allowContentAccess = true
            allowFileAccessFromFileURLs = false
            allowUniversalAccessFromFileURLs = false
            javaScriptCanOpenWindowsAutomatically = false
            setSupportMultipleWindows(false)
            mixedContentMode = android.webkit.WebSettings.MIXED_CONTENT_NEVER_ALLOW
        }
        webView.addJavascriptInterface(AndroidBridge(), "TrackportAndroid")
        webView.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(
                view: WebView,
                request: WebResourceRequest,
            ): Boolean {
                val uri = request.url
                if (
                    uri.scheme == "http" &&
                    uri.host == "127.0.0.1" &&
                    uri.port == serverPort
                ) {
                    return false
                }
                if (uri.scheme == "https") {
                    runCatching {
                        startActivity(Intent(Intent.ACTION_VIEW, uri))
                    }.onFailure {
                        Toast.makeText(this@MainActivity, "No app can open that link.", Toast.LENGTH_SHORT)
                            .show()
                    }
                }
                return true
            }
        }
        webView.webChromeClient = object : WebChromeClient() {
            override fun onShowFileChooser(
                view: WebView?,
                callback: ValueCallback<Array<Uri>>,
                params: FileChooserParams?,
            ): Boolean {
                pendingFileChooser?.onReceiveValue(null)
                pendingFileChooser = callback
                val allowMultiple =
                    params?.mode == WebChromeClient.FileChooserParams.MODE_OPEN_MULTIPLE
                val picker = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "audio/*"
                    putExtra(Intent.EXTRA_ALLOW_MULTIPLE, allowMultiple)
                }
                return try {
                    @Suppress("DEPRECATION")
                    startActivityForResult(picker, REQUEST_AUDIO)
                    true
                } catch (_: Exception) {
                    pendingFileChooser = null
                    callback.onReceiveValue(null)
                    Toast.makeText(
                        this@MainActivity,
                        "The audio file picker could not be opened.",
                        Toast.LENGTH_SHORT,
                    ).show()
                    false
                }
            }

            override fun onJsAlert(
                view: WebView?,
                url: String?,
                message: String?,
                result: JsResult,
            ): Boolean {
                AlertDialog.Builder(this@MainActivity)
                    .setMessage(message.orEmpty())
                    .setPositiveButton("OK") { _, _ -> result.confirm() }
                    .setOnCancelListener { result.cancel() }
                    .show()
                return true
            }

            override fun onJsConfirm(
                view: WebView?,
                url: String?,
                message: String?,
                result: JsResult,
            ): Boolean {
                AlertDialog.Builder(this@MainActivity)
                    .setMessage(message.orEmpty())
                    .setPositiveButton("Continue") { _, _ -> result.confirm() }
                    .setNegativeButton("Cancel") { _, _ -> result.cancel() }
                    .setOnCancelListener { result.cancel() }
                    .show()
                return true
            }
        }

        startLocalService()
    }

    @Deprecated("Use the Android system back gesture.")
    override fun onBackPressed() {
        if (::webView.isInitialized && webView.canGoBack()) {
            webView.goBack()
        } else {
            super.onBackPressed()
        }
    }

    private fun startLocalService() {
        background.execute {
            try {
                val webRoot = File(filesDir, "trackport-www")
                if (webRoot.exists() && !webRoot.deleteRecursively()) {
                    throw IOException("Could not clear Trackport's previous web assets.")
                }
                copyAssetFolder("www", webRoot)
                if (!Python.isStarted()) {
                    Python.start(AndroidPlatform(applicationContext))
                }
                val server = Python.getInstance().getModule("trackport_server")
                serverPort = server.callAttr(
                    "start_server",
                    webRoot.absolutePath,
                    cacheDir.absolutePath,
                ).toInt()
                mainHandler.post {
                    if (!isFinishing) {
                        setContentView(webView)
                        webView.loadUrl("http://127.0.0.1:$serverPort/")
                    }
                }
            } catch (error: Exception) {
                val message = error.message?.take(180) ?: error.javaClass.simpleName
                mainHandler.post {
                    loadingView.text =
                        "Trackport could not start its on-device server.\n\n$message\n\n" +
                        "No audio was sent anywhere. Close the app and try again."
                }
            }
        }
    }

    private fun copyAssetFolder(assetPath: String, target: File) {
        val assetFile = try {
            assets.open(assetPath)
        } catch (_: IOException) {
            null
        }

        if (assetFile != null) {
            assetFile.use { input ->
                if (target.isDirectory && !target.deleteRecursively()) {
                    throw IOException("Could not replace a stale Trackport web asset.")
                }
                val parent = target.parentFile
                if (parent != null && !parent.exists() && !parent.mkdirs()) {
                    throw IOException("Could not create Trackport's web asset directory.")
                }
                FileOutputStream(target).use { output -> input.copyTo(output) }
            }
            return
        }

        val children = assets.list(assetPath).orEmpty()
        if (children.isEmpty()) {
            throw IOException("Packaged Trackport asset could not be opened: $assetPath")
        }
        if (target.exists() && !target.isDirectory && !target.delete()) {
            throw IOException("Could not replace a stale Trackport web asset.")
        }
        if (!target.exists() && !target.mkdirs()) {
            throw IOException("Could not create Trackport's web asset directory.")
        }
        children.forEach { child ->
            copyAssetFolder("$assetPath/$child", File(target, child))
        }
    }

    private inner class AndroidBridge {
        private val preferences by lazy {
            getSharedPreferences("trackport-proxy", MODE_PRIVATE)
        }
        private val allowedKeys = setOf(
            "proxyEnabled",
            "proxyUrl",
            "proxyUsername",
            "proxyPassword",
        )

        @JavascriptInterface
        fun getPreference(key: String): String? {
            if (key !in allowedKeys) return null
            return preferences.getString(key, null)
        }

        @JavascriptInterface
        fun setPreference(key: String, value: String) {
            if (key !in allowedKeys) return
            preferences.edit().putString(key, value).apply()
        }

        @JavascriptInterface
        fun saveExport(exportId: String, requestedName: String) {
            if (!exportId.matches(Regex("[A-Za-z0-9_-]{1,64}"))) return
            val exports = File(cacheDir, "exports").canonicalFile
            val file = File(exports, "export-$exportId").canonicalFile
            if (file.parentFile != exports || !file.isFile) {
                mainHandler.post {
                    Toast.makeText(
                        this@MainActivity,
                        "The prepared export could not be found.",
                        Toast.LENGTH_LONG,
                    ).show()
                }
                return
            }
            val safeName = requestedName
                .substringAfterLast('/')
                .substringAfterLast('\\')
                .replace(Regex("[<>:\"/\\\\|?*\\u0000-\\u001f\\u007f]"), " ")
                .trim()
                .take(120)
                .ifBlank { "Trackport-export" }
            mainHandler.post { beginExport(file, safeName) }
        }
    }

    private fun beginExport(file: File, name: String) {
        pendingExport = PendingExport(file, name)
        val mimeType = if (name.endsWith(".zip", ignoreCase = true)) {
            "application/zip"
        } else {
            "audio/mpeg"
        }
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = mimeType
            putExtra(Intent.EXTRA_TITLE, name)
        }
        try {
            @Suppress("DEPRECATION")
            startActivityForResult(intent, REQUEST_EXPORT)
        } catch (_: Exception) {
            pendingExport = null
            file.delete()
            Toast.makeText(this, "The save dialog could not be opened.", Toast.LENGTH_LONG).show()
        }
    }

    @Deprecated("Handled through Activity Result APIs by WebView file-picker callbacks.")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_AUDIO) {
            val callback = pendingFileChooser ?: return
            pendingFileChooser = null
            val results = if (resultCode == RESULT_OK && data != null) {
                val clip = data.clipData
                when {
                    clip != null -> Array(clip.itemCount) { index -> clip.getItemAt(index).uri }
                    data.data != null -> arrayOf(data.data!!)
                    else -> null
                }
            } else {
                null
            }
            callback.onReceiveValue(results)
            return
        }
        if (requestCode == REQUEST_EXPORT) {
            val export = pendingExport
            pendingExport = null
            if (export == null) return
            if (resultCode != RESULT_OK || data?.data == null) {
                export.file.delete()
                return
            }
            try {
                val output = contentResolver.openOutputStream(data.data!!, "w")
                    ?: error("The selected location could not be opened.")
                output.use { destination ->
                    export.file.inputStream().use { input -> input.copyTo(destination) }
                }
                Toast.makeText(this, "Saved ${export.name}", Toast.LENGTH_SHORT).show()
            } catch (_: Exception) {
                Toast.makeText(
                    this,
                    "Trackport could not write the export to that location.",
                    Toast.LENGTH_LONG,
                ).show()
            } finally {
                export.file.delete()
            }
        }
    }

    override fun onDestroy() {
        pendingFileChooser?.onReceiveValue(null)
        pendingFileChooser = null
        background.execute {
            runCatching {
                if (Python.isStarted()) {
                    Python.getInstance().getModule("trackport_server").callAttr("stop_server")
                }
            }
        }
        background.shutdown()
        if (::webView.isInitialized) {
            webView.stopLoading()
            webView.removeJavascriptInterface("TrackportAndroid")
            webView.destroy()
        }
        super.onDestroy()
    }

    companion object {
        private const val REQUEST_AUDIO = 4101
        private const val REQUEST_EXPORT = 4102
    }
}
