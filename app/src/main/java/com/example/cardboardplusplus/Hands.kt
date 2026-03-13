package com.example.cardboardplusplus

import android.content.Context
import android.opengl.GLES20
import android.util.Log
import com.google.mediapipe.framework.image.MPImage
import com.google.mediapipe.tasks.core.BaseOptions
import com.google.mediapipe.tasks.core.Delegate
import com.google.mediapipe.tasks.vision.core.RunningMode
import com.google.mediapipe.tasks.vision.handlandmarker.HandLandmarker
import com.google.mediapipe.tasks.vision.handlandmarker.HandLandmarkerResult
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors

/**
 * MediaPipe hand tracking singleton.
 * Detects 21 landmarks per hand using GPU acceleration.
 */
object Hands {
    init {
        System.loadLibrary("mediapipe_tasks_vision_jni")
    }

    /** Single hand detection result with landmarks and handedness. */
    data class HandData(val landmarks: List<Landmark>, val handedness: String)

    /** 3D hand landmark point. Coordinates normalized [0,1]. */
    data class Landmark(val x: Float, val y: Float, val z: Float)

    /** Current detected hands. */
    var handResults: List<HandData> = emptyList()

    var handLandmarker: HandLandmarker? = null
    private val inferenceExecutor = Executors.newFixedThreadPool(2)

    /** Initializes MediaPipe hand landmarker with GPU delegate. */
    fun initializeMediaPipe(context: Context) {
        try {
            val baseOptions = BaseOptions.builder()
                .setModelAssetPath("hand_landmarker.task")
                .setDelegate(Delegate.GPU)
                .build()
            
            val options = HandLandmarker.HandLandmarkerOptions.builder()
                .setBaseOptions(baseOptions)
                .setRunningMode(RunningMode.LIVE_STREAM)
                .setNumHands(2)
                .setResultListener { result, timestamp ->
                    processResult(result)
                }
                .build()
            
            handLandmarker = HandLandmarker.createFromOptions(context, options)
            Log.i("Hands", "HandLandmarker initialized with GPU")
        } catch (e: Exception) {
            Log.e("Hands", "HandLandmarker init failed: ${e.message}")
        }
    }

    /** Runs hand detection asynchronously on the given image. */
    fun detectAsync(mpImage: MPImage, timestamp: Long) {
        handLandmarker?.detectAsync(mpImage, timestamp)
    }

    private fun processResult(result: HandLandmarkerResult?) {
        if (result == null) {
            handResults = emptyList()
            return
        }

        val hands = mutableListOf<HandData>()
        val landmarks = result.landmarks()
        val handedness = result.handednesses()

        for (i in landmarks.indices) {
            val handLandmarks = landmarks[i]
            val handType = if (handedness[i][0].categoryName() == "Left") "Left" else "Right"

            val landmarkList = mutableListOf<Landmark>()
            for (j in handLandmarks.indices) {
                val lm = handLandmarks[j]
                landmarkList.add(Landmark(lm.x(), lm.y(), lm.z()))
            }
            hands.add(HandData(landmarkList, handType))
        }
        handResults = hands
    }

    /** Renders detected hand landmarks and connections as lines/points using OpenGL. */
    fun drawHandLandmarks(lineProgram: Int) {
        val hands = handResults
        if (hands.isEmpty()) return

        GLES20.glUseProgram(lineProgram)

        val handConnections = listOf(
            Pair(0, 1), Pair(1, 2), Pair(2, 3), Pair(3, 4),
            Pair(0, 5), Pair(5, 6), Pair(6, 7), Pair(7, 8),
            Pair(0, 9), Pair(9, 10), Pair(10, 11), Pair(11, 12),
            Pair(0, 13), Pair(13, 14), Pair(14, 15), Pair(15, 16),
            Pair(0, 17), Pair(17, 18), Pair(18, 19), Pair(19, 20),
            Pair(5, 9), Pair(9, 13), Pair(13, 17)
        )

        for (hand in hands) {
            for (connection in handConnections) {
                if (connection.first < hand.landmarks.size && connection.second < hand.landmarks.size) {
                    val start = hand.landmarks[connection.first]
                    val end = hand.landmarks[connection.second]

                    val lineCoords = floatArrayOf(
                        start.x * 2 - 1, -(start.y * 2 - 1), 0f,
                        end.x * 2 - 1, -(end.y * 2 - 1), 0f
                    )
                    val lineBuffer = ByteBuffer.allocateDirect(lineCoords.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
                    lineBuffer.put(lineCoords).position(0)

                    val posHandle = GLES20.glGetAttribLocation(lineProgram, "aPosition")
                    GLES20.glEnableVertexAttribArray(posHandle)
                    GLES20.glVertexAttribPointer(posHandle, 3, GLES20.GL_FLOAT, false, 0, lineBuffer)
                    GLES20.glLineWidth(3f)
                    GLES20.glDrawArrays(GLES20.GL_LINES, 0, 2)
                    GLES20.glDisableVertexAttribArray(posHandle)
                }
            }

            for (landmark in hand.landmarks) {
                val pointCoords = floatArrayOf(landmark.x * 2 - 1, -(landmark.y * 2 - 1), 0f)
                val pointBuffer = ByteBuffer.allocateDirect(pointCoords.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
                pointBuffer.put(pointCoords).position(0)

                val posHandle = GLES20.glGetAttribLocation(lineProgram, "aPosition")
                GLES20.glEnableVertexAttribArray(posHandle)
                GLES20.glVertexAttribPointer(posHandle, 3, GLES20.GL_FLOAT, false, 0, pointBuffer)
                GLES20.glDrawArrays(GLES20.GL_POINTS, 0, 1)
                GLES20.glDisableVertexAttribArray(posHandle)
            }
        }
    }
}
