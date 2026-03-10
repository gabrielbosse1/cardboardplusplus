package com.example.cardboardplusplus

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.sqrt

object Tracker : SensorEventListener {
    private var sensorManager: SensorManager? = null
    private var gyroSensor: Sensor? = null
    private var accelSensor: Sensor? = null
    private var rotSensor: Sensor? = null

    var gyroscope = FloatArray(3)
        private set

    var accelerometer = FloatArray(3)
        private set

    var rotation = FloatArray(3)
        private set

    var position = FloatArray(3)
        private set

    private val rotationMatrix = FloatArray(9)
    private val orientationAngles = FloatArray(3)
    private val accelWorld = FloatArray(3)

    private var lastTimestamp: Long = 0

    // TODO: Turn this into a setting.
    private const val accelerometerDeadzone = 0.2f

    fun initialize(context: Context) {
        sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
        gyroSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        rotSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
        accelSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION)
    }

    fun start() {
        gyroSensor?.let {
            sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME)
        }
        rotSensor?.let {
            sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME)
        }
        accelSensor?.let {
            sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME)
        }
        lastTimestamp = System.nanoTime()
    }

    fun stop() {
        sensorManager?.unregisterListener(this)
    }
    // TODO: Reset position on long press.
    fun resetPosition() {
        position[0] = 0f
        position[1] = 0f
        position[2] = 0f
    }
    // TODO: Make a toggle for the 6DoF.
    override fun onSensorChanged(event: SensorEvent) {
        val dt = (System.nanoTime() - lastTimestamp) / 1e9f
        lastTimestamp = System.nanoTime()

        when (event.sensor.type) {
            Sensor.TYPE_GYROSCOPE -> {
                gyroscope[0] = event.values[0]
                gyroscope[1] = event.values[1]
                gyroscope[2] = event.values[2]
            }
            Sensor.TYPE_ROTATION_VECTOR -> {
                SensorManager.getRotationMatrixFromVector(rotationMatrix, event.values)
                SensorManager.getOrientation(rotationMatrix, orientationAngles)

                rotation[0] = Math.toDegrees(orientationAngles[1].toDouble()).toFloat()
                rotation[1] = Math.toDegrees(orientationAngles[2].toDouble()).toFloat()
                rotation[2] = Math.toDegrees(orientationAngles[0].toDouble()).toFloat()
            }
            Sensor.TYPE_LINEAR_ACCELERATION -> {
                for (i in event.values.indices) {
                    if (abs(event.values[i]) > accelerometerDeadzone) {
                        accelerometer[i] = event.values[i]
                    } else {
                        accelerometer[i] = 0f
                    }
                }

                val rotationVecAvailable = rotationMatrix.any { it != 0f }
                if (rotationVecAvailable) {
                    accelWorld[0] = rotationMatrix[0] * accelerometer[0] + rotationMatrix[1] * accelerometer[1] + rotationMatrix[2] * accelerometer[2]
                    accelWorld[1] = rotationMatrix[3] * accelerometer[0] + rotationMatrix[4] * accelerometer[1] + rotationMatrix[5] * accelerometer[2]
                    accelWorld[2] = rotationMatrix[6] * accelerometer[0] + rotationMatrix[7] * accelerometer[1] + rotationMatrix[8] * accelerometer[2]

                    position[0] += accelWorld[0] * dt
                    position[1] += accelWorld[1] * dt
                    position[2] += accelWorld[2] * dt
                } else {
                    position[0] += accelerometer[0] * dt
                    position[1] += accelerometer[1] * dt
                    position[2] += accelerometer[2] * dt
                }
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
}
