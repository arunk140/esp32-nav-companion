package com.navesp32.companion

import android.os.Bundle
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import org.json.JSONObject
import java.util.concurrent.Executors

// Scans the QR on the device screen: {"n","m","s","t"} -> saved bond.
class QrScannerActivity : AppCompatActivity() {

    companion object {
        const val TAG = "QrScanner"
    }

    private val exec = Executors.newSingleThreadExecutor()
    private var done = false
    private val scanner = BarcodeScanning.getClient(
        BarcodeScannerOptions.Builder()
            .setBarcodeFormats(Barcode.FORMAT_QR_CODE).build()
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val preview = PreviewView(this)
        setContentView(preview)
        val provider = ProcessCameraProvider.getInstance(this)
        provider.addListener({
            val analysis = ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .build()
            analysis.setAnalyzer(exec) { img: ImageProxy ->
                process(img)
            }
            try {
                provider.get().unbindAll()
                val previewUseCase = androidx.camera.core.Preview.Builder().build()
                previewUseCase.setSurfaceProvider(preview.surfaceProvider)
                provider.get().bindToLifecycle(
                    this, CameraSelector.DEFAULT_BACK_CAMERA,
                    previewUseCase,
                    analysis
                )
            } catch (e: Exception) {
                Log.w(TAG, "bind: $e")
                finish()
            }
        }, ContextCompat.getMainExecutor(this))
    }

    @androidx.camera.core.ExperimentalGetImage
    private fun process(img: ImageProxy) {
        if (done) {
            img.close()
            return
        }
        val media = img.image
        if (media == null) {
            img.close()
            return
        }
        val input = InputImage.fromMediaImage(media, img.imageInfo.rotationDegrees)
        scanner.process(input)
            .addOnSuccessListener { codes ->
                for (c in codes) {
                    val raw = c.rawValue ?: continue
                    if (handle(raw)) break
                }
            }
            .addOnCompleteListener { img.close() }
    }

    private fun handle(raw: String): Boolean {
        try {
            val j = JSONObject(raw)
            val mac = j.getString("m")
            val name = j.optString("n", "NAV-????")
            val svc = j.optString("s", BleManager.SVC_UUID)
            val token = j.getString("t")
            if (!mac.matches(Regex("([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}"))) return false
            if (token.length < 4) return false
            Prefs.saveBond(this, Prefs.Bond(mac.uppercase(), name, svc, token))
            Prefs.setServiceEnabled(this, true)
            done = true
            Log.i(TAG, "bond saved $name $mac")
            NavService.start(this, fresh = true)
            finish()
            return true
        } catch (e: Exception) {
            Log.w(TAG, "bad qr: $e")
            return false
        }
    }

    override fun onDestroy() {
        exec.shutdown()
        super.onDestroy()
    }
}
