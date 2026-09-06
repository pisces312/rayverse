package com.rayverse.rayman;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.util.SparseIntArray;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import org.libsdl.app.SDLActivity;

/**
 * Virtual gamepad overlay for Rayman 1.
 *
 * Landscape: D-pad on the left, action buttons on the right (over the game).
 * Portrait:  game is letterboxed to the top, controls live in the free space
 *            below so they never cover the picture.
 *
 * A small settings button opens the orientation menu.
 *
 * Buttons map to SDL keyboard scancodes via SDLActivity.onNativeKeyDown/Up.
 */
public class GamepadOverlay extends View {
    /* Button IDs */
    private static final int BTN_UP = 0;
    private static final int BTN_DOWN = 1;
    private static final int BTN_LEFT = 2;
    private static final int BTN_RIGHT = 3;
    private static final int BTN_A = 4;     /* Jump */
    private static final int BTN_B = 5;     /* Attack/Fist */
    private static final int BTN_X = 6;     /* Grab/Helicopter */
    private static final int BTN_Y = 7;     /* Run toggle */
    private static final int BTN_COUNT = 8;

    /* SDL key mappings for each button */
    private static final int[] KEY_CODES = {
        KeyEvent.KEYCODE_DPAD_UP,
        KeyEvent.KEYCODE_DPAD_DOWN,
        KeyEvent.KEYCODE_DPAD_LEFT,
        KeyEvent.KEYCODE_DPAD_RIGHT,
        KeyEvent.KEYCODE_SPACE,      /* A - Jump */
        KeyEvent.KEYCODE_SHIFT_LEFT, /* B - Attack */
        KeyEvent.KEYCODE_ESCAPE,     /* X - Grab */
        KeyEvent.KEYCODE_TAB,        /* Y - Run toggle */
    };

    private static final String[] LABELS = {
        "▲", "▼", "◀", "▶",
        "A", "B", "X", "Y",
    };

    /* Logical game resolution aspect (320x200) for portrait letterboxing. */
    private static final float GAME_ASPECT = 320f / 200f; /* 1.6 */

    /* Press counters (multi-touch: two fingers on one button) */
    private final int[] pressCount = new int[BTN_COUNT];
    /* Pointer ID -> button mapping */
    private final SparseIntArray pointerMap = new SparseIntArray();
    /* Button hit areas (set by layout) */
    private final Rect[] hitAreas = new Rect[BTN_COUNT];

    private boolean landscape = true;

    /* Settings button */
    public interface OnSettingsClickListener { void onSettingsClick(); }
    private OnSettingsClickListener settingsListener;
    private final Rect settingsRect = new Rect();
    private int settingsIconSize;
    private boolean settingsPressed;

    /* Paints */
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint gearPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float density;
    private float cornerRadius;

    public GamepadOverlay(Context context) {
        super(context);
        setFocusable(false);
        setClickable(false);

        density = getResources().getDisplayMetrics().density;
        cornerRadius = 14 * density;

        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(2 * density);

        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setFakeBoldText(true);

        gearPaint.setStyle(Paint.Style.STROKE);
        gearPaint.setStrokeWidth(2 * density);
        gearPaint.setColor(0x99FFFFFF);
    }

    public void setOnSettingsClickListener(OnSettingsClickListener l) {
        settingsListener = l;
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        super.onLayout(changed, left, top, right, bottom);
        int w = right - left;
        int h = bottom - top;
        if (w == 0 || h == 0) return;
        landscape = (w >= h);
        if (landscape) layoutLandscape(w, h);
        else           layoutPortrait(w, h);
        layoutSettingsButton(w, h);
    }

    private void layoutLandscape(int w, int h) {
        int btnSize = Math.min(w, h) / 5;
        int pad = btnSize / 2;

        int dpadCenterX = w / 4;
        int dpadCenterY = h / 2;
        hitAreas[BTN_UP]    = rect(dpadCenterX - btnSize/2, dpadCenterY - btnSize - pad, dpadCenterX + btnSize/2, dpadCenterY - pad);
        hitAreas[BTN_DOWN]  = rect(dpadCenterX - btnSize/2, dpadCenterY + pad,           dpadCenterX + btnSize/2, dpadCenterY + btnSize + pad);
        hitAreas[BTN_LEFT]  = rect(dpadCenterX - btnSize - pad, dpadCenterY - btnSize/2, dpadCenterX - pad,       dpadCenterY + btnSize/2);
        hitAreas[BTN_RIGHT] = rect(dpadCenterX + pad,           dpadCenterY - btnSize/2, dpadCenterX + btnSize + pad, dpadCenterY + btnSize/2);

        int actCenterX = w * 3 / 4;
        int actCenterY = h / 2;
        hitAreas[BTN_A] = rect(actCenterX - btnSize/2, actCenterY - btnSize - pad, actCenterX + btnSize/2, actCenterY - pad);
        hitAreas[BTN_B] = rect(actCenterX + pad,           actCenterY - btnSize/2, actCenterX + btnSize + pad, actCenterY + btnSize/2);
        hitAreas[BTN_X] = rect(actCenterX - btnSize - pad, actCenterY - btnSize/2, actCenterX - pad,           actCenterY + btnSize/2);
        hitAreas[BTN_Y] = rect(actCenterX - btnSize/2,     actCenterY + pad,       actCenterX + btnSize/2,     actCenterY + btnSize + pad);
    }

    private void layoutPortrait(int w, int h) {
        /* Game is letterboxed to the top (height = w / 1.6). Controls fill the
         * remaining space below so they never overlap the picture. */
        int gameH = (int) (w / GAME_ASPECT);
        int freeTop = Math.min(gameH, h);
        int freeH = Math.max(h - freeTop, 0);
        int centerY = freeTop + freeH / 2;
        int btnSize = Math.min(w / 7, Math.max(freeH / 4, 1));
        int pad = btnSize / 2;

        int dpadCenterX = w / 3;
        int dpadCenterY = centerY;
        hitAreas[BTN_UP]    = rect(dpadCenterX - btnSize/2, dpadCenterY - btnSize - pad, dpadCenterX + btnSize/2, dpadCenterY - pad);
        hitAreas[BTN_DOWN]  = rect(dpadCenterX - btnSize/2, dpadCenterY + pad,           dpadCenterX + btnSize/2, dpadCenterY + btnSize + pad);
        hitAreas[BTN_LEFT]  = rect(dpadCenterX - btnSize - pad, dpadCenterY - btnSize/2, dpadCenterX - pad,       dpadCenterY + btnSize/2);
        hitAreas[BTN_RIGHT] = rect(dpadCenterX + pad,           dpadCenterY - btnSize/2, dpadCenterX + btnSize + pad, dpadCenterY + btnSize/2);

        int actCenterX = w * 2 / 3;
        int actCenterY = centerY;
        hitAreas[BTN_A] = rect(actCenterX - btnSize/2, actCenterY - btnSize - pad, actCenterX + btnSize/2, actCenterY - pad);
        hitAreas[BTN_B] = rect(actCenterX + pad,           actCenterY - btnSize/2, actCenterX + btnSize + pad, actCenterY + btnSize/2);
        hitAreas[BTN_X] = rect(actCenterX - btnSize - pad, actCenterY - btnSize/2, actCenterX - pad,           actCenterY + btnSize/2);
        hitAreas[BTN_Y] = rect(actCenterX - btnSize/2,     actCenterY + pad,       actCenterX + btnSize/2,     actCenterY + btnSize + pad);
    }

    private void layoutSettingsButton(int w, int h) {
        settingsIconSize = (int) (Math.min(w, h) * 0.10f);
        int margin = (int) (12 * density);
        settingsRect.set(w - settingsIconSize - margin, margin,
                         w - margin, settingsIconSize + margin);
    }

    private Rect rect(int l, int t, int r, int b) {
        return new Rect(l, t, r, b);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        for (int i = 0; i < BTN_COUNT; i++) {
            Rect r = hitAreas[i];
            if (r == null) continue;
            boolean pressed = pressCount[i] > 0;
            drawButton(canvas, r, LABELS[i], pressed);
        }
        drawSettingsButton(canvas);
    }

    private void drawButton(Canvas canvas, Rect r, String label, boolean pressed) {
        RectF rf = new RectF(r);

        fillPaint.setStyle(Paint.Style.FILL);
        fillPaint.setColor(pressed ? 0x55FFFFFF : 0x22FFFFFF);
        canvas.drawRoundRect(rf, cornerRadius, cornerRadius, fillPaint);

        borderPaint.setColor(pressed ? 0xCCFFFFFF : 0x55FFFFFF);
        canvas.drawRoundRect(rf, cornerRadius, cornerRadius, borderPaint);

        textPaint.setColor(pressed ? 0xFFFFFFFF : 0x99FFFFFF);
        textPaint.setTextSize(r.height() * 0.42f);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        float cx = r.exactCenterX();
        float cy = r.exactCenterY() - (fm.ascent + fm.descent) / 2f;
        canvas.drawText(label, cx, cy, textPaint);
    }

    private void drawSettingsButton(Canvas canvas) {
        if (settingsRect.isEmpty()) return;

        boolean pressed = settingsPressed;
        RectF rf = new RectF(settingsRect);
        fillPaint.setStyle(Paint.Style.FILL);
        fillPaint.setColor(pressed ? 0x55FFFFFF : 0x22FFFFFF);
        canvas.drawRoundRect(rf, cornerRadius, cornerRadius, fillPaint);

        borderPaint.setColor(pressed ? 0xCCFFFFFF : 0x55FFFFFF);
        canvas.drawRoundRect(rf, cornerRadius, cornerRadius, borderPaint);

        /* Simple gear icon drawn with strokes */
        float cx = settingsRect.exactCenterX();
        float cy = settingsRect.exactCenterY();
        float outer = Math.min(settingsRect.width(), settingsRect.height()) * 0.28f;
        float inner = outer * 0.6f;
        int teeth = 8;

        Path path = new Path();
        for (int i = 0; i < teeth * 2; i++) {
            double angle = Math.PI * 2 * i / (teeth * 2);
            float r = (i % 2 == 0) ? outer : inner;
            float x = cx + (float) (Math.cos(angle) * r);
            float y = cy + (float) (Math.sin(angle) * r);
            if (i == 0) path.moveTo(x, y);
            else        path.lineTo(x, y);
        }
        path.close();
        gearPaint.setColor(pressed ? 0xFFFFFFFF : 0x99FFFFFF);
        canvas.drawPath(path, gearPaint);
        canvas.drawCircle(cx, cy, inner * 0.35f, gearPaint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        try {
            return handleTouch(event);
        } catch (UnsatisfiedLinkError e) {
            return true;
        }
    }

    private boolean handleTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerIndex = event.getActionIndex();
        int pointerId = event.getPointerId(pointerIndex);

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = event.getX(pointerIndex);
                float y = event.getY(pointerIndex);
                if (settingsRect.contains((int) x, (int) y)) {
                    settingsPressed = true;
                    if (action == MotionEvent.ACTION_DOWN && settingsListener != null) {
                        settingsListener.onSettingsClick();
                    }
                    invalidate();
                    break;
                }
                int btn = hitTest(x, y);
                if (btn >= 0) {
                    pointerMap.put(pointerId, btn);
                    pressButton(btn);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < event.getPointerCount(); i++) {
                    int pid = event.getPointerId(i);
                    float x = event.getX(i);
                    float y = event.getY(i);

                    if (settingsRect.contains((int) x, (int) y)) {
                        if (!settingsPressed) {
                            settingsPressed = true;
                            invalidate();
                        }
                        continue;
                    } else if (settingsPressed && pid == event.getPointerId(0)) {
                        settingsPressed = false;
                        invalidate();
                    }

                    int oldBtn = pointerMap.get(pid, -1);
                    int newBtn = hitTest(x, y);
                    if (oldBtn != newBtn) {
                        if (oldBtn >= 0) releaseButton(oldBtn);
                        if (newBtn >= 0) pressButton(newBtn);
                        if (newBtn >= 0) pointerMap.put(pid, newBtn);
                        else pointerMap.delete(pid);
                    }
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                float x = event.getX(pointerIndex);
                float y = event.getY(pointerIndex);
                if (settingsRect.contains((int) x, (int) y)) {
                    settingsPressed = false;
                    invalidate();
                    break;
                }
                int btn = pointerMap.get(pointerId, -1);
                if (btn >= 0) {
                    releaseButton(btn);
                    pointerMap.delete(pointerId);
                }
                if (pointerMap.size() == 0) forceReleaseAll();
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                settingsPressed = false;
                forceReleaseAll();
                pointerMap.clear();
                break;
            }
        }
        return true;
    }

    private int hitTest(float x, float y) {
        for (int i = 0; i < BTN_COUNT; i++) {
            if (hitAreas[i] != null && hitAreas[i].contains((int) x, (int) y)) return i;
        }
        return -1;
    }

    private void pressButton(int btn) {
        pressCount[btn]++;
        if (pressCount[btn] == 1) SDLActivity.onNativeKeyDown(KEY_CODES[btn]);
        invalidate();
    }

    private void releaseButton(int btn) {
        pressCount[btn]--;
        if (pressCount[btn] <= 0) {
            pressCount[btn] = 0;
            SDLActivity.onNativeKeyUp(KEY_CODES[btn]);
        }
        invalidate();
    }

    private void forceReleaseAll() {
        for (int i = 0; i < BTN_COUNT; i++) {
            if (pressCount[i] > 0) {
                pressCount[i] = 0;
                SDLActivity.onNativeKeyUp(KEY_CODES[i]);
            }
        }
        invalidate();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        forceReleaseAll();
        pointerMap.clear();
    }
}
