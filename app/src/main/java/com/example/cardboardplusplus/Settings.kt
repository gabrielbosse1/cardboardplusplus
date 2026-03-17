package com.example.cardboardplusplus

import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.graphics.Color
import android.view.GestureDetector
import android.view.MotionEvent
import com.google.vr.sdk.base.GvrView

class Settings(private val activity: MainActivity, private val gvrView: GvrView?) {

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

    fun setupGestureDetector() {
        gestureDetector = GestureDetector(activity, object : GestureDetector.SimpleOnGestureListener() {
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
        val scrollView = ScrollView(activity)
        scrollView.layoutParams = android.view.ViewGroup.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
            android.view.ViewGroup.LayoutParams.MATCH_PARENT
        )
        scrollView.setBackgroundColor(Color.argb(200, 0, 0, 0))

        val linearLayout = LinearLayout(activity)
        linearLayout.orientation = LinearLayout.VERTICAL
        linearLayout.layoutParams = android.view.ViewGroup.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        )
        linearLayout.setPadding(32, 32, 32, 32)

        val titleLayout = LinearLayout(activity)
        titleLayout.orientation = LinearLayout.HORIZONTAL
        titleLayout.layoutParams = android.view.ViewGroup.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        )

        val titleText = TextView(activity)
        titleText.text = "Settings"
        titleText.textSize = 24f
        titleText.setTextColor(Color.WHITE)
        titleText.layoutParams = LinearLayout.LayoutParams(0, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, 1f)

        val closeButton = Button(activity)
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
        val row = LinearLayout(activity)
        row.orientation = LinearLayout.HORIZONTAL
        row.layoutParams = LinearLayout.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply {
            topMargin = 16
        }

        val labelView = TextView(activity)
        labelView.text = "$label: "
        labelView.textSize = 16f
        labelView.setTextColor(Color.WHITE)
        labelView.layoutParams = LinearLayout.LayoutParams(
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        )

        val valueView = TextView(activity)
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

    fun updateSettingsValues() {
        if (settingsView?.visibility != View.VISIBLE) return

        activity.runOnUiThread {
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
}
