package com.example.cardboardplusplus

import android.opengl.GLES30

/** OpenGL ES 3.0 shader programs for VR rendering. */
object Shaders {
    const val TEX_VERTEX_SHADER = "#version 300 es\nuniform mat4 uMVPMatrix;in vec4 aPosition;in vec2 aTexCoord;out vec2 vTexCoord;void main(){gl_Position=uMVPMatrix*aPosition;vTexCoord=aTexCoord;}"
    const val TEX_FRAGMENT_SHADER = "#version 300 es\nprecision mediump float;in vec2 vTexCoord;uniform sampler2D sTexture;out vec4 fragColor;void main(){fragColor=texture(sTexture,vTexCoord);}"

    const val LINE_VERTEX_SHADER = "#version 300 es\nin vec4 aPosition;void main(){gl_Position=aPosition;}"
    const val LINE_FRAGMENT_SHADER = "#version 300 es\nprecision mediump float;out vec4 fragColor;void main(){fragColor=vec4(0.0,1.0,0.0,1.0);}"

    /** Creates shader program for rendering camera texture to screen. */
    fun createTexProgram(): Int {
        val vs = GLES30.glCreateShader(GLES30.GL_VERTEX_SHADER)
        GLES30.glShaderSource(vs, TEX_VERTEX_SHADER)
        GLES30.glCompileShader(vs)

        val fs = GLES30.glCreateShader(GLES30.GL_FRAGMENT_SHADER)
        GLES30.glShaderSource(fs, TEX_FRAGMENT_SHADER)
        GLES30.glCompileShader(fs)

        val prog = GLES30.glCreateProgram()
        GLES30.glAttachShader(prog, vs)
        GLES30.glAttachShader(prog, fs)
        GLES30.glLinkProgram(prog)

        return prog
    }

    /** Creates shader program for drawing hand landmark lines/points. */
    fun createLineProgram(): Int {
        val vs = GLES30.glCreateShader(GLES30.GL_VERTEX_SHADER)
        GLES30.glShaderSource(vs, LINE_VERTEX_SHADER)
        GLES30.glCompileShader(vs)

        val fs = GLES30.glCreateShader(GLES30.GL_FRAGMENT_SHADER)
        GLES30.glShaderSource(fs, LINE_FRAGMENT_SHADER)
        GLES30.glCompileShader(fs)

        val prog = GLES30.glCreateProgram()
        GLES30.glAttachShader(prog, vs)
        GLES30.glAttachShader(prog, fs)
        GLES30.glLinkProgram(prog)

        return prog
    }
}
