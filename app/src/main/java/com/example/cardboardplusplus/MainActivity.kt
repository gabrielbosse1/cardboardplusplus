package com.example.cardboardplusplus

import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.opengl.GLES30
import android.opengl.Matrix
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import androidx.appcompat.app.AppCompatActivity
import com.google.mediapipe.framework.image.BitmapImageBuilder
import com.google.mediapipe.framework.image.MPImage
import com.google.vr.sdk.base.GvrView
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MainActivity : AppCompatActivity(), GvrView.StereoRenderer {

    private var camera: Camera? = null
    private var currentBitmap: Bitmap? = null
    private var textureId = 0
    private var texProgram = 0
    private var lineProgram = 0
    private var gvrView: GvrView? = null
    private var settings: Settings? = null

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
        gvrView?.setEGLContextClientVersion(3)
        gvrView?.setRenderer(this)
        gvrView?.setTransitionViewEnabled(true)

        setupGestureDetector()
        
        Hands.initializeMediaPipe(this)
        Tracker.initialize(this)
        
        camera = Camera(this)
        camera?.onBitmapAvailable = { bitmap ->
            runOnUiThread {
                currentBitmap = bitmap
                inference(bitmap)
            }
        }
        
        if (camera?.hasPermission() == true) {
            camera?.openCamera()
        } else {
            camera?.requestPermission(this)
        }
    }

    private fun inference(bitmap: Bitmap) {
        try {
            val mpImage: MPImage = BitmapImageBuilder(bitmap).build()
            val timestamp = System.currentTimeMillis()
            Hands.detectAsync(mpImage, timestamp)
        } catch (e: Exception) {
            android.util.Log.e("MainActivity", "Hand detection error", e)
        }
    }

    private fun setupGestureDetector() {
        settings = Settings(this, gvrView)
        settings?.setupGestureDetector()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == Camera.CAMERA_PERMISSION_REQUEST_CODE && grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            camera?.openCamera()
        }
    }

    override fun onResume() {
        super.onResume()
        camera?.onResume()
        Tracker.resetPosition()
        Tracker.start()
    }

    override fun onPause() {
        Tracker.stop()
        camera?.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        super.onDestroy()
        Hands.handLandmarker?.close()
        camera?.release()
    }

    override fun onSurfaceCreated(config: javax.microedition.khronos.egl.EGLConfig?) {
        GLES30.glClearColor(0f, 0f, 0f, 1f)

        val textures = IntArray(1)
        GLES30.glGenTextures(1, textures, 0)
        textureId = textures[0]

        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, textureId)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)

        texProgram = Shaders.createTexProgram()
        lineProgram = Shaders.createLineProgram()
    }

    override fun onSurfaceChanged(width: Int, height: Int) {
        GLES30.glViewport(0, 0, width, height)
    }

    override fun onNewFrame(headTransform: com.google.vr.sdk.base.HeadTransform?) {}

    override fun onDrawEye(eye: com.google.vr.sdk.base.Eye?) {
        GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT or GLES30.GL_DEPTH_BUFFER_BIT)

        val bitmap = currentBitmap
        if (bitmap != null && !bitmap.isRecycled) {
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, textureId)
            android.opengl.GLUtils.texImage2D(GLES30.GL_TEXTURE_2D, 0, bitmap, 0)
            currentBitmap = null
        }

        drawTexture()
        Hands.drawHandLandmarks(lineProgram)
        
        settings?.updateSettingsValues()
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

        GLES30.glUseProgram(texProgram)

        val posHandle = GLES30.glGetAttribLocation(texProgram, "aPosition")
        GLES30.glEnableVertexAttribArray(posHandle)
        GLES30.glVertexAttribPointer(posHandle, 3, GLES30.GL_FLOAT, false, 0, vertexBuffer)

        val texHandle = GLES30.glGetAttribLocation(texProgram, "aTexCoord")
        GLES30.glEnableVertexAttribArray(texHandle)
        GLES30.glVertexAttribPointer(texHandle, 2, GLES30.GL_FLOAT, false, 0, texBuffer)

        val mvpMatrix = FloatArray(16)
        Matrix.setIdentityM(mvpMatrix, 0)
        val mvpHandle = GLES30.glGetUniformLocation(texProgram, "uMVPMatrix")
        GLES30.glUniformMatrix4fv(mvpHandle, 1, false, mvpMatrix, 0)

        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4)

        GLES30.glDisableVertexAttribArray(posHandle)
        GLES30.glDisableVertexAttribArray(texHandle)
    }

    fun getCamera(): Camera? = camera

    fun getCurrentBitmap(): Bitmap? = currentBitmap

    fun getTextureId(): Int = textureId
}
