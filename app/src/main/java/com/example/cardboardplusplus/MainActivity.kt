package com.example.cardboardplusplus

import android.Manifest
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
import android.opengl.GLES20
import android.opengl.Matrix
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.widget.Toast
import android.view.View
import android.view.ViewGroup
import android.view.GestureDetector
import android.view.MotionEvent
import android.widget.ScrollView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Button
import android.graphics.Color
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.mediapipe.framework.image.BitmapImageBuilder
import com.google.mediapipe.framework.image.MPImage
import com.google.vr.sdk.base.GvrView
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

/**
 * Main VR activity.
 * Handles camera preview, hand tracking, 6DoF tracking, and VR rendering.
 */
class MainActivity : AppCompatActivity(), GvrView.StereoRenderer {

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null
    private val cameraOpenCloseLock = Semaphore(1)
    private val inferenceExecutor = Executors.newFixedThreadPool(2)
    private var targetWidth = 0
    private var targetHeight = 0

    private var currentBitmap: Bitmap? = null
    private var textureId = 0
    private var texProgram = 0
    private var lineProgram = 0
    private var gvrView: GvrView? = null
    private var wasCameraOpen = false
    private var settingsView: ScrollView? = null
    private var gestureDetector: GestureDetector? = null
    
    private var posXText: TextView? = null
    private var posYText: TextView? = null
    private var posZText: TextView? = null
    private var rotXText: TextView? = null
    private var rotYText: TextView? = null
    private var rotZText: TextView? = null
    private var accXText: TextView? = null
    private var accYText: TextView? = null
    private var accZText: TextView? = null
    private var gyroXText: TextView? = null
    private var gyroYText: TextView? = null
    private var gyroZText: TextView? = null
    
    private var lastClickTime: Long = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        window.decorView.systemUiVisibility = (
            android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            or android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
            or android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            or android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            or android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            or android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        )
        
        setContentView(R.layout.activity_main)

        gvrView = findViewById(R.id.gvr_view)
        gvrView?.setEGLContextClientVersion(2)
        gvrView?.setRenderer(this)
        gvrView?.setTransitionViewEnabled(true)

        setupGestureDetector()
        
        Hands.initializeMediaPipe(this)
        Tracker.initialize(this)
        
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            openCamera()
        } else {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.CAMERA), 1001)
        }
    }

    private fun setupGestureDetector() {
        gestureDetector = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
                toggleSettings()
                return true
            }
        })
        
        gvrView?.setOnTouchListener { _, event ->
            gestureDetector?.onTouchEvent(event)
            true
        }
    }

    private fun toggleSettings() {
        if (settingsView == null) {
            createSettingsView()
        }
        
        if (settingsView?.visibility == View.VISIBLE) {
            settingsView?.visibility = View.GONE
        } else {
            settingsView?.visibility = View.VISIBLE
        }
    }

    private fun createSettingsView() {
        val scrollView = ScrollView(this)
        scrollView.layoutParams = android.view.ViewGroup.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
            android.view.ViewGroup.LayoutParams.MATCH_PARENT
        )
        scrollView.setBackgroundColor(Color.argb(200, 0, 0, 0))
        
        val linearLayout = LinearLayout(this)
        linearLayout.orientation = LinearLayout.VERTICAL
        linearLayout.layoutParams = android.view.ViewGroup.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        )
        linearLayout.setPadding(32, 32, 32, 32)
        
        val titleLayout = LinearLayout(this)
        titleLayout.orientation = LinearLayout.HORIZONTAL
        titleLayout.layoutParams = android.view.ViewGroup.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        )
        
        val titleText = TextView(this)
        titleText.text = "Settings"
        titleText.textSize = 24f
        titleText.setTextColor(Color.WHITE)
        titleText.layoutParams = LinearLayout.LayoutParams(0, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        
        val closeButton = Button(this)
        closeButton.text = "X"
        closeButton.setOnClickListener { toggleSettings() }
        
        titleLayout.addView(titleText)
        titleLayout.addView(closeButton)
        linearLayout.addView(titleLayout)
        
        addLabeledValue(linearLayout, "Position X", posXText)
        addLabeledValue(linearLayout, "Position Y", posYText)
        addLabeledValue(linearLayout, "Position Z", posZText)
        
        addLabeledValue(linearLayout, "Rotation X", rotXText)
        addLabeledValue(linearLayout, "Rotation Y", rotYText)
        addLabeledValue(linearLayout, "Rotation Z", rotZText)
        
        addLabeledValue(linearLayout, "Accelerometer X", accXText)
        addLabeledValue(linearLayout, "Accelerometer Y", accYText)
        addLabeledValue(linearLayout, "Accelerometer Z", accZText)
        
        addLabeledValue(linearLayout, "Gyroscope X", gyroXText)
        addLabeledValue(linearLayout, "Gyroscope Y", gyroYText)
        addLabeledValue(linearLayout, "Gyroscope Z", gyroZText)
        
        scrollView.addView(linearLayout)
        
        (gvrView as? ViewGroup)?.addView(scrollView)
        settingsView = scrollView
        settingsView?.visibility = View.GONE
    }

    private fun addLabeledValue(parent: LinearLayout, label: String, textView: TextView?) {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.layoutParams = LinearLayout.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply {
            topMargin = 16
        }
        
        val labelView = TextView(this)
        labelView.text = "$label: "
        labelView.textSize = 16f
        labelView.setTextColor(Color.WHITE)
        labelView.layoutParams = LinearLayout.LayoutParams(
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        )
        
        val valueView = TextView(this)
        valueView.text = "0.0"
        valueView.textSize = 16f
        valueView.setTextColor(Color.CYAN)
        valueView.layoutParams = LinearLayout.LayoutParams(
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        )
        
        row.addView(labelView)
        row.addView(valueView)
        parent.addView(row)
        
        when (label) {
            "Position X" -> posXText = valueView
            "Position Y" -> posYText = valueView
            "Position Z" -> posZText = valueView
            "Rotation X" -> rotXText = valueView
            "Rotation Y" -> rotYText = valueView
            "Rotation Z" -> rotZText = valueView
            "Accelerometer X" -> accXText = valueView
            "Accelerometer Y" -> accYText = valueView
            "Accelerometer Z" -> accZText = valueView
            "Gyroscope X" -> gyroXText = valueView
            "Gyroscope Y" -> gyroYText = valueView
            "Gyroscope Z" -> gyroZText = valueView
        }
    }

    private fun updateSettingsValues() {
        if (settingsView?.visibility != View.VISIBLE) return
        
        runOnUiThread {
            posXText?.text = String.format("%.3f", Tracker.position[0])
            posYText?.text = String.format("%.3f", Tracker.position[1])
            posZText?.text = String.format("%.3f", Tracker.position[2])
            
            rotXText?.text = String.format("%.3f", Tracker.rotation[0])
            rotYText?.text = String.format("%.3f", Tracker.rotation[1])
            rotZText?.text = String.format("%.3f", Tracker.rotation[2])
            
            accXText?.text = String.format("%.3f", Tracker.accelerometer[0])
            accYText?.text = String.format("%.3f", Tracker.accelerometer[1])
            accZText?.text = String.format("%.3f", Tracker.accelerometer[2])
            
            gyroXText?.text = String.format("%.3f", Tracker.gyroscope[0])
            gyroYText?.text = String.format("%.3f", Tracker.gyroscope[1])
            gyroZText?.text = String.format("%.3f", Tracker.gyroscope[2])
        }
    }

    private fun openCamera() {
        val cameraManager = getSystemService(CAMERA_SERVICE) as CameraManager
        try {
            // TODO: Add a dropdown menu to select the camera ID.
            val cameraId = cameraManager.cameraIdList[0]
            val characteristics = cameraManager.getCameraCharacteristics(cameraId)
            val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)

            // TODO: Add a dropdown menu to select the camera resolution.
            val previewSizes = map?.getOutputSizes(ImageFormat.YUV_420_888)
            val previewSize = android.util.Size(640, 480)

            imageReader = ImageReader.newInstance(previewSize.width, previewSize.height, ImageFormat.YUV_420_888, 2)
            targetWidth = previewSize.width
            targetHeight = previewSize.height
            imageReader?.setOnImageAvailableListener({ reader ->
                val image = reader.acquireLatestImage()
                if (image != null) {
                    currentBitmap = Images.yuvToBitmap(image.planes, image.width, image.height)
                    
                    val bitmap = currentBitmap
                    if (bitmap != null) {
                        inferenceExecutor.execute {
                            try {
                                val mpImage: MPImage = BitmapImageBuilder(bitmap).build()
                                val timestamp = System.currentTimeMillis()
                                Hands.detectAsync(mpImage, timestamp)
                            } catch (e: Exception) {
                                android.util.Log.e("MainActivity", "Hand detection error", e)
        }
    }
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
            android.util.Log.e("MainActivity", "Camera error", e)
        } catch (e: SecurityException) {
            android.util.Log.e("MainActivity", "Permission error", e)
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
                        android.util.Log.e("MainActivity", "Preview error", e)
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    android.util.Log.e("MainActivity", "Configuration failed")
                }
            }, backgroundHandler)
        } catch (e: CameraAccessException) {
            android.util.Log.e("MainActivity", "Camera access error", e)
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
            android.util.Log.e("MainActivity", "Background thread error", e)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 1001 && grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            openCamera()
        } else {
            Toast.makeText(this, "Camera permission denied", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onResume() {
        super.onResume()
        startBackgroundThread()
        Tracker.resetPosition()
        Tracker.start()
        
        if (wasCameraOpen) {
            wasCameraOpen = false
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                openCamera()
            }
        }
    }

    override fun onPause() {
        Tracker.stop()
        wasCameraOpen = cameraDevice != null
        closeCamera()
        stopBackgroundThread()
        super.onPause()
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
            android.util.Log.e("MainActivity", "Camera close error", e)
        } finally {
            cameraOpenCloseLock.release()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        inferenceExecutor.shutdown()
        Hands.handLandmarker?.close()
    }

    override fun onSurfaceCreated(config: javax.microedition.khronos.egl.EGLConfig?) {
        GLES20.glClearColor(0f, 0f, 0f, 1f)

        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        textureId = textures[0]

        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        texProgram = Shaders.createTexProgram()
        lineProgram = Shaders.createLineProgram()
    }

    override fun onSurfaceChanged(width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
    }

    override fun onNewFrame(headTransform: com.google.vr.sdk.base.HeadTransform?) {}

    override fun onDrawEye(eye: com.google.vr.sdk.base.Eye?) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)

        val bitmap = currentBitmap
        if (bitmap != null && !bitmap.isRecycled) {
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
            android.opengl.GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0)
            currentBitmap = null
        }

        drawTexture()
        Hands.drawHandLandmarks(lineProgram)
        
        updateSettingsValues()
    }

    override fun onFinishFrame(viewport: com.google.vr.sdk.base.Viewport?) {}

    override fun onRendererShutdown() {}

    private fun drawTexture() {
        val vertexCoords = floatArrayOf(-1f, -1f, 0f, 1f, -1f, 0f, -1f, 1f, 0f, 1f, 1f, 0f)
        val texCoords = floatArrayOf(0f, 1f, 1f, 1f, 0f, 0f, 1f, 0f)

        val vertexBuffer = ByteBuffer.allocateDirect(vertexCoords.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
        vertexBuffer.put(vertexCoords).position(0)

        val texBuffer = ByteBuffer.allocateDirect(texCoords.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
        texBuffer.put(texCoords).position(0)

        GLES20.glUseProgram(texProgram)

        val posHandle = GLES20.glGetAttribLocation(texProgram, "aPosition")
        GLES20.glEnableVertexAttribArray(posHandle)
        GLES20.glVertexAttribPointer(posHandle, 3, GLES20.GL_FLOAT, false, 0, vertexBuffer)

        val texHandle = GLES20.glGetAttribLocation(texProgram, "aTexCoord")
        GLES20.glEnableVertexAttribArray(texHandle)
        GLES20.glVertexAttribPointer(texHandle, 2, GLES20.GL_FLOAT, false, 0, texBuffer)

        val mvpMatrix = FloatArray(16)
        Matrix.setIdentityM(mvpMatrix, 0)
        val mvpHandle = GLES20.glGetUniformLocation(texProgram, "uMVPMatrix")
        GLES20.glUniformMatrix4fv(mvpHandle, 1, false, mvpMatrix, 0)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        GLES20.glDisableVertexAttribArray(posHandle)
        GLES20.glDisableVertexAttribArray(texHandle)
    }
}
