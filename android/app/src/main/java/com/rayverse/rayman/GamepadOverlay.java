package com.rayverse.rayman;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.util.SparseIntArray;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Toast;
import org.libsdl.app.SDLActivity;

/**
 * Virtual gamepad overlay for Rayman 1.
 *
 * Landscape: D-pad on the bottom-left, action buttons on the bottom-right.
 * Portrait:  game is letterboxed to the top, controls live in the free space
 *            below so they never cover the picture.
 *
 * Button positions can be customized by dragging while in edit mode.
 * A small settings button opens the orientation menu (or exits edit mode).
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

    /* SharedPreferences keys for custom layout */
    private static final String PREFS = "rayverse_prefs";
    private static final String KEY_BTN_X = "btn_";
    private static final String KEY_BTN_Y = "_y";
    private static final String KEY_BTN_SIZE = "_size";
    private static final String KEY_LAYOUT_VERSION = "layout_version";

    /* Press counters (multi-touch: two fingers on one button) */
    private final int[] pressCount = new int[BTN_COUNT];
    /* Pointer ID -> button mapping */
    private final SparseIntArray pointerMap = new SparseIntArray();
    /* Button hit areas (set by layout) */
    private final Rect[] hitAreas = new Rect[BTN_COUNT];

    private boolean landscape = true;

    /* Custom layout state */
    private final float[] customCx = new float[BTN_COUNT];
    private final float[] customCy = new float[BTN_COUNT];
    private final float[] customSize = new float[BTN_COUNT];
    private boolean hasCustomLayout = false;
    private boolean editMode = false;
    private int dragButtonId = -1;
    private float dragOffsetX, dragOffsetY;

    public interface OnEditModeChangeListener {
        void onEditModeChanged(boolean editing);
    }
    private OnEditModeChangeListener editListener;

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
    private final Paint editHintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
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

        editHintPaint.setTextAlign(Paint.Align.CENTER);
        editHintPaint.setTextSize(18 * density);
        editHintPaint.setColor(0xFFFFFFFF);

        loadCustomLayout();
    }

    public void setOnSettingsClickListener(OnSettingsClickListener l) {
        settingsListener = l;
    }

    public void setOnEditModeChangeListener(OnEditModeChangeListener l) {
        editListener = l;
    }

    public boolean isEditMode() {
        return editMode;
    }

    public void setEditMode(boolean enabled) {
        if (editMode == enabled) return;
        editMode = enabled;
        if (editMode) {
            ensureCustomSnapshot();
            Toast.makeText(getContext(), "拖动按钮调整位置，点击齿轮完成", Toast.LENGTH_LONG).show();
        } else {
            saveCustomLayout();
        }
        invalidate();
        if (editListener != null) editListener.onEditModeChanged(editMode);
    }

    public void resetLayout() {
        SharedPreferences.Editor editor = getContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit();
        for (int i = 0; i < BTN_COUNT; i++) {
            editor.remove(KEY_BTN_X + i);
            editor.remove(KEY_BTN_Y + i);
            editor.remove(KEY_BTN_SIZE + i);
            customCx[i] = customCy[i] = customSize[i] = 0f;
        }
        editor.remove(KEY_LAYOUT_VERSION);
        editor.apply();
        hasCustomLayout = false;
        dragButtonId = -1;
        requestLayout();
        invalidate();
    }

    private void loadCustomLayout() {
        SharedPreferences prefs = getContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        hasCustomLayout = false;
        for (int i = 0; i < BTN_COUNT; i++) {
            String kx = KEY_BTN_X + i;
            String ky = KEY_BTN_Y + i;
            String ks = KEY_BTN_SIZE + i;
            if (prefs.contains(kx) && prefs.contains(ky) && prefs.contains(ks)) {
                customCx[i] = prefs.getFloat(kx, 0.5f);
                customCy[i] = prefs.getFloat(ky, 0.5f);
                customSize[i] = prefs.getFloat(ks, 0.1f);
                hasCustomLayout = true;
            } else {
                customCx[i] = customCy[i] = customSize[i] = 0f;
            }
        }
    }

    private void saveCustomLayout() {
        if (!hasCustomLayout) return;
        SharedPreferences.Editor editor = getContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit();
        for (int i = 0; i < BTN_COUNT; i++) {
            editor.putFloat(KEY_BTN_X + i, customCx[i]);
            editor.putFloat(KEY_BTN_Y + i, customCy[i]);
            editor.putFloat(KEY_BTN_SIZE + i, customSize[i]);
        }
        editor.putInt(KEY_LAYOUT_VERSION, 1);
        editor.apply();
    }

    /* Take current default layout and store it as the editable custom layout */
    private void ensureCustomSnapshot() {
        if (hasCustomLayout) return;
        int w = getWidth();
        int h = getHeight();
        if (w == 0 || h == 0) return;
        for (int i = 0; i < BTN_COUNT; i++) {
            Rect r = hitAreas[i];
            if (r == null) continue;
            customCx[i] = r.exactCenterX() / w;
            customCy[i] = r.exactCenterY() / h;
            customSize[i] = (float) r.width() / Math.min(w, h);
        }
        hasCustomLayout = true;
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        super.onLayout(changed, left, top, right, bottom);
        int w = right - left;
        int h = bottom - top;
        if (w == 0 || h == 0) return;
        landscape = (w >= h);
        if (hasCustomLayout) {
            layoutFromCustom(w, h);
        } else {
            if (landscape) layoutLandscape(w, h);
            else           layoutPortrait(w, h);
        }
        layoutSettingsButton(w, h);
    }

    private void layoutFromCustom(int w, int h) {
        for (int i = 0; i < BTN_COUNT; i++) {
            int size = (int) (customSize[i] * Math.min(w, h));
            int cx = (int) (customCx[i] * w);
            int cy = (int) (customCy[i] * h);
            setButtonRect(i, cx, cy, size);
        }
    }

    private void setButtonRect(int id, int cx, int cy, int size) {
        hitAreas[id] = rect(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2);
    }

    /*
     * Default landscape layout: D-pad bottom-left, actions bottom-right.
     */
    private void layoutLandscape(int w, int h) {
        int btnSize = (int) (Math.min(w, h) * 0.13f);
        int pad = btnSize / 2;

        int dpadCenterX = (int) (w * 0.20f);
        int dpadCenterY = (int) (h * 0.72f);
        hitAreas[BTN_UP]    = rect(dpadCenterX - btnSize/2, dpadCenterY - btnSize - pad, dpadCenterX + btnSize/2, dpadCenterY - pad);
        hitAreas[BTN_DOWN]  = rect(dpadCenterX - btnSize/2, dpadCenterY + pad,           dpadCenterX + btnSize/2, dpadCenterY + btnSize + pad);
        hitAreas[BTN_LEFT]  = rect(dpadCenterX - btnSize - pad, dpadCenterY - btnSize/2, dpadCenterX - pad,       dpadCenterY + btnSize/2);
        hitAreas[BTN_RIGHT] = rect(dpadCenterX + pad,           dpadCenterY - btnSize/2, dpadCenterX + btnSize + pad, dpadCenterY + btnSize/2);

        int actCenterX = (int) (w * 0.80f);
        int actCenterY = (int) (h * 0.72f);
        hitAreas[BTN_A] = rect(actCenterX - btnSize/2, actCenterY - btnSize - pad, actCenterX + btnSize/2, actCenterY - pad);
        hitAreas[BTN_B] = rect(actCenterX + pad,           actCenterY - btnSize/2, actCenterX + btnSize + pad, actCenterY + btnSize/2);
        hitAreas[BTN_X] = rect(actCenterX - btnSize - pad, actCenterY - btnSize/2, actCenterX - pad,           actCenterY + btnSize/2);
        hitAreas[BTN_Y] = rect(actCenterX - btnSize/2,     actCenterY + pad,       actCenterX + btnSize/2,     actCenterY + btnSize + pad);
    }

    /*
     * Default portrait layout: controls in the free space below the letterboxed game.
     */
    private void layoutPortrait(int w, int h) {
        int gameH = (int) (w / GAME_ASPECT);
        int freeTop = Math.min(gameH, h);
        int freeH = Math.max(h - freeTop, 0);
        int centerY = freeTop + freeH / 2;
        int btnSize = Math.min(w / 8, Math.max(freeH / 6, 1));
        int pad = btnSize / 2;

        int dpadCenterX = (int) (w * 0.22f);
        hitAreas[BTN_UP]    = rect(dpadCenterX - btnSize/2, centerY - btnSize - pad, dpadCenterX + btnSize/2, centerY - pad);
        hitAreas[BTN_DOWN]  = rect(dpadCenterX - btnSize/2, centerY + pad,           dpadCenterX + btnSize/2, centerY + btnSize + pad);
        hitAreas[BTN_LEFT]  = rect(dpadCenterX - btnSize - pad, centerY - btnSize/2, dpadCenterX - pad,       centerY + btnSize/2);
        hitAreas[BTN_RIGHT] = rect(dpadCenterX + pad,           centerY - btnSize/2, dpadCenterX + btnSize + pad, centerY + btnSize/2);

        int actCenterX = (int) (w * 0.78f);
        hitAreas[BTN_A] = rect(actCenterX - btnSize/2, centerY - btnSize - pad, actCenterX + btnSize/2, centerY - pad);
        hitAreas[BTN_B] = rect(actCenterX + pad,           centerY - btnSize/2, actCenterX + btnSize + pad, centerY + btnSize/2);
        hitAreas[BTN_X] = rect(actCenterX - btnSize - pad, centerY - btnSize/2, actCenterX - pad,           centerY + btnSize/2);
        hitAreas[BTN_Y] = rect(actCenterX - btnSize/2,     centerY + pad,       actCenterX + btnSize/2,     centerY + btnSize + pad);
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
            boolean dragging = (i == dragButtonId);
            drawButton(canvas, r, LABELS[i], pressed, dragging);
        }
        drawSettingsButton(canvas);
        if (editMode) drawEditHint(canvas);
    }

    private void drawButton(Canvas canvas, Rect r, String label, boolean pressed, boolean dragging) {
        RectF rf = new RectF(r);

        fillPaint.setStyle(Paint.Style.FILL);
        fillPaint.setColor(dragging ? 0x55FFFF00 : (pressed ? 0x55FFFFFF : 0x22FFFFFF));
        canvas.drawRoundRect(rf, cornerRadius, cornerRadius, fillPaint);

        borderPaint.setColor(pressed || dragging ? 0xCCFFFFFF : 0x55FFFFFF);
        canvas.drawRoundRect(rf, cornerRadius, cornerRadius, borderPaint);

        textPaint.setColor(pressed || dragging ? 0xFFFFFFFF : 0x99FFFFFF);
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

        float cx = settingsRect.exactCenterX();
        float cy = settingsRect.exactCenterY();
        float outer = Math.min(settingsRect.width(), settingsRect.height()) * 0.28f;
        float inner = outer * 0.6f;
        int teeth = 8;

        Path path = new Path();
        for (int i = 0; i < teeth * 2; i++) {
            double angle = Math.PI * 2 * i / (teeth * 2);
            float rr = (i % 2 == 0) ? outer : inner;
            float x = cx + (float) (Math.cos(angle) * rr);
            float y = cy + (float) (Math.sin(angle) * rr);
            if (i == 0) path.moveTo(x, y);
            else        path.lineTo(x, y);
        }
        path.close();
        gearPaint.setColor(pressed ? 0xFFFFFFFF : 0x99FFFFFF);
        canvas.drawPath(path, gearPaint);
        canvas.drawCircle(cx, cy, inner * 0.35f, gearPaint);
    }

    private void drawEditHint(Canvas canvas) {
        String hint = "拖动按钮调整位置，点击齿轮完成";
        float x = canvas.getWidth() / 2f;
        float y = canvas.getHeight() * 0.15f;
        canvas.drawText(hint, x, y, editHintPaint);
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
                    if (action == MotionEvent.ACTION_DOWN) {
                        if (editMode) {
                            setEditMode(false);
                        } else if (settingsListener != null) {
                            settingsListener.onSettingsClick();
                        }
                    }
                    invalidate();
                    break;
                }
                if (editMode) {
                    int btn = hitTest(x, y);
                    if (btn >= 0) {
                        dragButtonId = btn;
                        dragOffsetX = x - hitAreas[btn].exactCenterX();
                        dragOffsetY = y - hitAreas[btn].exactCenterY();
                        ensureCustomSnapshot();
                        invalidate();
                    }
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
                if (editMode && dragButtonId >= 0) {
                    int idx = event.findPointerIndex(event.getPointerId(0));
                    if (idx >= 0) {
                        float x = event.getX(idx);
                        float y = event.getY(idx);
                        moveDraggedButton(x - dragOffsetX, y - dragOffsetY);
                    }
                    break;
                }
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
                if (editMode) {
                    if (dragButtonId >= 0) {
                        saveCustomLayout();
                        dragButtonId = -1;
                        invalidate();
                    }
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
                if (editMode) {
                    dragButtonId = -1;
                }
                forceReleaseAll();
                pointerMap.clear();
                invalidate();
                break;
            }
        }
        return true;
    }

    private void moveDraggedButton(float cx, float cy) {
        if (dragButtonId < 0) return;
        int w = getWidth();
        int h = getHeight();
        if (w == 0 || h == 0) return;
        float nx = Math.max(0f, Math.min(1f, cx / w));
        float ny = Math.max(0f, Math.min(1f, cy / h));
        customCx[dragButtonId] = nx;
        customCy[dragButtonId] = ny;
        int size = (int) (customSize[dragButtonId] * Math.min(w, h));
        setButtonRect(dragButtonId, (int) cx, (int) cy, size);
        invalidate();
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
        if (editMode) saveCustomLayout();
        forceReleaseAll();
        pointerMap.clear();
    }
}
