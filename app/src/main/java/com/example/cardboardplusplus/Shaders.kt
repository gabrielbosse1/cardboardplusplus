package com.example.cardboardplusplus

import android.opengl.GLES20

/** OpenGL ES 2.0 shader programs for VR rendering. */
object Shaders {
    const val TEX_VERTEX_SHADER = "uniform mat4 uMVPMatrix;attribute vec4 aPosition;attribute vec2 aTexCoord;varying vec2 vTexCoord;void main(){gl_Position=uMVPMatrix*aPosition;vTexCoord=aTexCoord;}"
    const val TEX_FRAGMENT_SHADER = "precision mediump float;varying vec2 vTexCoord;uniform sampler2D sTexture;void main(){gl_FragColor=texture2D(sTexture,vTexCoord);}"

    const val LINE_VERTEX_SHADER = "attribute vec4 aPosition;void main(){gl_Position=aPosition;}"
    const val LINE_FRAGMENT_SHADER = "precision mediump float;void main(){gl_FragColor=vec4(0.0,1.0,0.0,1.0);}"

    /** Creates shader program for rendering camera texture to screen. */
    fun createTexProgram(): Int {
        val vs = GLES20.glCreateShader(GLES20.GL_VERTEX_SHADER)
        GLES20.glShaderSource(vs, TEX_VERTEX_SHADER)
        GLES20.glCompileShader(vs)

        val fs = GLES20.glCreateShader(GLES20.GL_FRAGMENT_SHADER)
        GLES20.glShaderSource(fs, TEX_FRAGMENT_SHADER)
        GLES20.glCompileShader(fs)

        val prog = GLES20.glCreateProgram()
        GLES20.glAttachShader(prog, vs)
        GLES20.glAttachShader(prog, fs)
        GLES20.glLinkProgram(prog)

        return prog
    }

    /** Creates shader program for drawing hand landmark lines/points. */
    fun createLineProgram(): Int {
        val vs = GLES20.glCreateShader(GLES20.GL_VERTEX_SHADER)
        GLES20.glShaderSource(vs, LINE_VERTEX_SHADER)
        GLES20.glCompileShader(vs)

        val fs = GLES20.glCreateShader(GLES20.GL_FRAGMENT_SHADER)
        GLES20.glShaderSource(fs, LINE_FRAGMENT_SHADER)
        GLES20.glCompileShader(fs)

        val prog = GLES20.glCreateProgram()
        GLES20.glAttachShader(prog, vs)
        GLES20.glAttachShader(prog, fs)
        GLES20.glLinkProgram(prog)

        return prog
    }
}
