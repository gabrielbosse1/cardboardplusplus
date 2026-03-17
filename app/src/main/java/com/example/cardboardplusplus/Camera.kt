package com.example.cardboardplusplus

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.ImageFormat
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.widget.Toast
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.util.concurrent.Executors
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

class Camera(private val context: Context) {

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null
    private val cameraOpenCloseLock = Semaphore(1)
    
    var targetWidth = 0
        private set
    var targetHeight = 0
        private set
    
    private var wasCameraOpen = false

    var onBitmapAvailable: ((Bitmap) -> Unit)? = null
    private val inferenceExecutor = Executors.newFixedThreadPool(2)

    fun hasPermission(): Boolean {
        return ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
    }

    fun requestPermission(activity: android.app.Activity) {
        ActivityCompat.requestPermissions(activity, arrayOf(Manifest.permission.CAMERA), CAMERA_PERMISSION_REQUEST_CODE)
    }

    fun onPermissionResult(granted: Boolean) {
        if (granted) {
            openCamera()
        } else {
            Toast.makeText(context, "Camera permission denied", Toast.LENGTH_SHORT).show()
        }
    }

    fun onResume() {
        startBackgroundThread()
        if (wasCameraOpen) {
            wasCameraOpen = false
            if (hasPermission()) {
                openCamera()
            }
        }
    }

    fun onPause() {
        wasCameraOpen = cameraDevice != null
        closeCamera()
        stopBackgroundThread()
    }

    fun openCamera() {
        if (!hasPermission()) return

        val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        try {
            val cameraId = cameraManager.cameraIdList[0]
            val characteristics = cameraManager.getCameraCharacteristics(cameraId)
            val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)

            val previewSizes = map?.getOutputSizes(ImageFormat.YUV_420_888)
            val previewSize = android.util.Size(640, 480)

            imageReader = ImageReader.newInstance(previewSize.width, previewSize.height, ImageFormat.YUV_420_888, 2)
            targetWidth = previewSize.width
            targetHeight = previewSize.height
            imageReader?.setOnImageAvailableListener({ reader ->
                val image = reader.acquireLatestImage()
                if (image != null) {
                    val bitmap = Images.yuvToBitmap(image.planes, image.width, image.height)
                    if (bitmap != null) {
                        onBitmapAvailable?.invoke(bitmap)
                    }
                    image.close()
                }
            }, backgroundHandler)

            if (!cameraOpenCloseLock.tryAcquire(2500, TimeUnit.MILLISECONDS)) {
                throw RuntimeException("Time out waiting to lock camera opening.")
            }

            cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    cameraOpenCloseLock.release()
                    cameraDevice = camera
                    createCameraPreviewSession()
                }

                override fun onDisconnected(camera: CameraDevice) {
                    cameraOpenCloseLock.release()
                    camera.close()
                    cameraDevice = null
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    cameraOpenCloseLock.release()
                    camera.close()
                    cameraDevice = null
                }
            }, backgroundHandler)

        } catch (e: CameraAccessException) {
            android.util.Log.e("Camera", "Camera error", e)
        } catch (e: SecurityException) {
            android.util.Log.e("Camera", "Permission error", e)
        }
    }

    private fun createCameraPreviewSession() {
        try {
            val surface = imageReader?.surface ?: return

            val previewRequestBuilder = cameraDevice?.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
            previewRequestBuilder?.addTarget(surface)

            cameraDevice?.createCaptureSession(listOf(surface), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    if (cameraDevice == null) return
                    captureSession = session
                    try {
                        previewRequestBuilder?.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                        session.setRepeatingRequest(previewRequestBuilder!!.build(), null, backgroundHandler)
                    } catch (e: CameraAccessException) {
                        android.util.Log.e("Camera", "Preview error", e)
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    android.util.Log.e("Camera", "Configuration failed")
                }
            }, backgroundHandler)
        } catch (e: CameraAccessException) {
            android.util.Log.e("Camera", "Camera access error", e)
        }
    }

    private fun startBackgroundThread() {
        backgroundThread = HandlerThread("CameraBackground").also { it.start() }
        backgroundHandler = Handler(backgroundThread!!.looper)
    }

    private fun stopBackgroundThread() {
        backgroundThread?.quitSafely()
        try {
            backgroundThread?.join()
            backgroundThread = null
            backgroundHandler = null
        } catch (e: InterruptedException) {
            android.util.Log.e("Camera", "Background thread error", e)
        }
    }

    private fun closeCamera() {
        try {
            cameraOpenCloseLock.acquire()
            captureSession?.close()
            captureSession = null
            cameraDevice?.close()
            cameraDevice = null
            imageReader?.close()
            imageReader = null
        } catch (e: InterruptedException) {
            android.util.Log.e("Camera", "Camera close error", e)
        } finally {
            cameraOpenCloseLock.release()
        }
    }

    fun release() {
        inferenceExecutor.shutdown()
        closeCamera()
        stopBackgroundThread()
    }

    fun isCameraOpen(): Boolean = cameraDevice != null

    companion object {
        const val CAMERA_PERMISSION_REQUEST_CODE = 1001
    }
}
