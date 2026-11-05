package dev.nyanroid

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.view.View
import kotlin.math.floor
import kotlin.math.min

/**
 * The virtual cursor, drawn over the game: the game's own cursor picture (from the engine, in the
 * game's pixels) or, where the game shows the system's arrow, a plain arrow. It is scaled with the
 * game's picture, as a cursor looks over the game in a window at its own size, and its hotspot sits
 * at [x], [y] (view pixels). It takes no touches.
 */
class CursorView(context: Context) : View(context) {
    /** The game's virtual screen, which the engine letterboxes into the view. */
    var screenWidth = 0
    var screenHeight = 0
    var x0 = 0f
        private set
    var y0 = 0f
        private set
    private var bitmap: Bitmap? = null
    private var hotX = 0
    private var hotY = 0
    var cx = 0f
        private set
    var cy = 0f
        private set
    private val bitmapPaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFFFFF.toInt(); style = Paint.Style.FILL }
    private val edge = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF000000.toInt(); style = Paint.Style.STROKE; strokeWidth = 1f; strokeJoin = Paint.Join.MITER
    }
    // Windows' standard arrow, its tip the hotspot (game pixels).
    private val arrow = Path().apply {
        moveTo(0f, 0f); lineTo(0f, 17f); lineTo(4f, 13f); lineTo(7f, 20f); lineTo(10f, 19f)
        lineTo(7f, 12f); lineTo(12f, 12f); close()
    }

    init {
        isClickable = false
        isFocusable = false
    }

    /** Whether the cursor shows. The view itself always stays visible: a view over the game's
     *  SurfaceView that only becomes visible later is never composited (the window's see-through
     *  region over the surface is worked out while it is hidden), so the cursor never showed when
     *  turned on after the game started. */
    var shown = false
        set(on) {
            field = on
            invalidate()
        }

    /** The game's picture's scale in this view, as the engine letterboxes it. */
    fun scale(): Float {
        if (screenWidth <= 0 || screenHeight <= 0 || width <= 0 || height <= 0) return resources.displayMetrics.density
        return min(width.toFloat() / screenWidth, height.toFloat() / screenHeight)
    }

    /** The picture's right / bottom edges (its left / top are [x0], [y0]). */
    fun x1() = x0 + screenWidth * scale()
    fun y1() = y0 + screenHeight * scale()

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        placePicture()
    }

    fun placePicture() {
        val s = scale()
        x0 = floor((width - screenWidth * s) / 2)
        y0 = floor((height - screenHeight * s) / 2)
    }

    /** [serial, width, height, hotX, hotY, ARGB...] from [NativeBridge.cursor]; [0]: the arrow. */
    fun setImage(data: IntArray) {
        bitmap?.recycle()
        bitmap = if (data.size >= 5 && data[1] > 0 && data[2] > 0 && data.size >= 5 + data[1] * data[2]) {
            hotX = data[3]
            hotY = data[4]
            Bitmap.createBitmap(data, 5, data[1], data[1], data[2], Bitmap.Config.ARGB_8888)
        } else {
            null
        }
        invalidate()
    }

    fun moveTo(x: Float, y: Float) {
        cx = x
        cy = y
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (!shown) return
        val s = scale()
        canvas.save()
        canvas.translate(cx, cy)
        canvas.scale(s, s)
        val b = bitmap
        if (b != null) {
            canvas.drawBitmap(b, -hotX.toFloat(), -hotY.toFloat(), bitmapPaint)
        } else {
            canvas.drawPath(arrow, fill)
            canvas.drawPath(arrow, edge)
        }
        canvas.restore()
    }
}
