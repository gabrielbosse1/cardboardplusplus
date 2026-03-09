package com.example.cardboardplusplus

import android.graphics.Bitmap
import android.media.Image
import android.util.Log

object Images {
    private var reusableBitmap: Bitmap? = null

    fun yuvToBitmap(planes: Array<Image.Plane>, width: Int, height: Int): Bitmap? {
        try {
            val yBuffer = planes[0].buffer
            val uBuffer = planes[1].buffer
            val vBuffer = planes[2].buffer

            val ySize = yBuffer.remaining()
            val uSize = uBuffer.remaining()
            val vSize = vBuffer.remaining()

            val nv21 = ByteArray(ySize + vSize + uSize)
            yBuffer.get(nv21, 0, ySize)
            vBuffer.get(nv21, ySize, vSize)
            uBuffer.get(nv21, ySize + vSize, uSize)

            if (reusableBitmap == null || reusableBitmap!!.width != width || reusableBitmap!!.height != height) {
                reusableBitmap?.recycle()
                reusableBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            }

            val argb = IntArray(width * height)
            decodeNV21toARGB(nv21, width, height, argb)
            reusableBitmap!!.setPixels(argb, 0, width, 0, 0, width, height)
            return reusableBitmap
        } catch (e: Exception) {
            Log.e("Images", "YUV conversion error", e)
            return null
        }
    }

    private fun decodeNV21toARGB(nv21: ByteArray, width: Int, height: Int, argb: IntArray) {
        val frameSize = width * height
        var y: Int
        var r: Int
        var g: Int
        var b: Int
        var u: Int
        var v: Int

        for (j in 0 until height) {
            val uvRow = j / 2
            for (i in 0 until width) {
                y = (0xff and nv21[j * width + i].toInt())
                if (y < 16) y = 16

                val uvIndex = frameSize + uvRow * width + (i / 2) * 2
                if (uvIndex + 1 < nv21.size) {
                    v = (0xff and nv21[uvIndex].toInt()) - 128
                    u = (0xff and nv21[uvIndex + 1].toInt()) - 128
                } else {
                    u = 0
                    v = 0
                }

                r = (y + 1.402 * v).toInt()
                g = (y - 0.344136 * u - 0.714136 * v).toInt()
                b = (y + 1.772 * u).toInt()

                r = r.coerceIn(0, 255)
                g = g.coerceIn(0, 255)
                b = b.coerceIn(0, 255)

                argb[j * width + i] = (0xff shl 24) or (r shl 16) or (g shl 8) or b
            }
        }
    }
}
