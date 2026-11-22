// [android] The launcher's menu (docs/ANDROID_LAUNCHER.md): a horizontal bar of categories and, under the selected
// one, a vertical list of its items - the feel of a handheld console's home menu, drawn by us on a Canvas (our own
// glyphs, the platform's font, no sounds). A choice item opens its options in the same column; Back closes them.
//
// Everything is laid out from the view's size and the window's insets, never from fixed dp guesses, so one layout
// serves a foldable folded (1080x2420, portrait or landscape) and unfolded (2352x2172).
// Input: touch (tap an item or a category, drag the list, swipe sideways for another category) and keys - D-pad or
// stick (the framework turns stick and hat motion into D-pad keys), A / Enter confirm, B / Escape back, L1 / R1
// categories, Start plays.
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextUtils;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.VelocityTracker;
import android.view.View;
import android.view.ViewConfiguration;

import java.io.File;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.function.BooleanSupplier;
import java.util.function.IntConsumer;
import java.util.function.IntSupplier;
import java.util.function.Supplier;

final class MenuView extends View {
    static final int G_PLAY = 0, G_DISC = 1, G_SETTINGS = 2, G_CONTROLS = 3, G_FRAMEGEN = 4, G_ABOUT = 5;
    static final int G_SAVES = 6; // [saves] SavesMenu's category

    interface Host {
        void play();

        /** [lcs] the header's game switch: game index (setGameSwitch's order) picked */
        default void pickGame(int index) {}

        /** The row's full explanation (long press, X on a pad, the row's "i"): its title, its full value and its help */
        default void showHelp(String title, String text) {}
    }

    /** One row. value / help / enabled / visible are read by refresh(), never while drawing. */
    static final class Item {
        final String title;
        Supplier<String> value, help;
        Supplier<File> picture; // a picture over the help text (a save's), null: none
        BooleanSupplier enabled, visible;
        Runnable action;
        // a choice: its options, the current one, and what picking one does
        String[] choices;
        IntSupplier current;
        IntConsumer choose;
        boolean primary;
        // refresh()'s copies
        String valueText = "", helpText = "";
        File pictureFile;
        boolean on = true;

        /** What the explanation dialog says: the full value line, then the help text; "" when there is nothing */
        String explanation() {
            String v = valueText == null ? "" : valueText.trim(), h = helpText == null ? "" : helpText.trim();
            if (v.isEmpty())
                return h;
            return h.isEmpty() ? v : v + "\n\n" + h;
        }

        boolean hasInfo() {
            return !helpText.isEmpty() || valueText.length() > 40;
        }

        Item(String title) {
            this.title = title;
        }

        Item value(Supplier<String> v) { value = v; return this; }
        Item help(Supplier<String> h) { help = h; return this; }
        Item help(String h) { help = () -> h; return this; }
        Item picture(Supplier<File> p) { picture = p; return this; }
        Item enabled(BooleanSupplier e) { enabled = e; return this; }
        Item visible(BooleanSupplier v) { visible = v; return this; }
        Item action(Runnable a) { action = a; return this; }
        Item primary() { primary = true; return this; }

        Item choice(String[] names, IntSupplier cur, IntConsumer pick) {
            choices = names;
            current = cur;
            choose = pick;
            if (value == null)
                value = () -> {
                    int i = cur.getAsInt();
                    return i >= 0 && i < names.length ? names[i] : "";
                };
            return this;
        }

        /** A choice whose options change (the connected controllers): read again each time it opens */
        Supplier<String[]> choicesNow;

        Item choice(Supplier<String[]> names, IntSupplier cur, IntConsumer pick) {
            choicesNow = names;
            return choice(names.get(), cur, pick);
        }
    }

    static final class Category {
        final String name;
        final int glyph;
        final List<Item> items = new ArrayList<>();
        final List<Item> shown = new ArrayList<>();
        int sel;

        Category(String name, int glyph) {
            this.name = name;
            this.glyph = glyph;
        }

        Item add(Item i) {
            items.add(i);
            return i;
        }
    }

    private final Host host;
    private final List<Category> cats = new ArrayList<>();
    private int cat;
    private float catPos;
    private Item sub; // the choice whose options are open
    private int subSel, subCurrent;
    private float scroll, scrollTarget, flingV;
    private boolean touchScrolled, gamepad;
    private String buildTag = "";

    // layout, from the size and insets (layout())
    private int insetL, insetT, insetR, insetB;
    private float T, icon, colX, spacing, barY, catLabelY, listTop, listBottom, rowH, textX, textRight, helpX, helpY,
            helpW, helpBottom, left, top, right, bottom;
    private boolean wide, helpBeside;

    private final TextPaint title = new TextPaint(Paint.ANTI_ALIAS_FLAG), small = new TextPaint(Paint.ANTI_ALIAS_FLAG),
            helpPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG), fill = new Paint(Paint.ANTI_ALIAS_FLAG),
            glowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF rect = new RectF();
    private StaticLayout helpLayout;
    private String helpLayoutText;
    private float helpLayoutWidth;
    private long lastFrame;
    private float flash; // a refused confirm (disabled item): a short pink flash

    // touch
    private final float slop;
    private float downX, downY, scrollAtDown;
    private int dragMode; // 0 undecided, 1 sideways (categories), 2 the list
    private VelocityTracker velocity;

    private final Runnable clock = new Runnable() {
        @Override
        public void run() {
            invalidate();
            postDelayed(this, 20_000);
        }
    };

    MenuView(Context c, Host host) {
        super(c);
        this.host = host;
        slop = ViewConfiguration.get(c).getScaledTouchSlop();
        setFocusable(true);
        setFocusableInTouchMode(true);
        setDefaultFocusHighlightEnabled(false); // the menu draws its own selection
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        title.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        small.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        helpPaint.setTypeface(Typeface.create("sans-serif-light", Typeface.NORMAL));
    }

    Category category(String name, int glyph) {
        Category c = new Category(name, glyph);
        cats.add(c);
        return c;
    }

    void setBuildTag(String s) {
        buildTag = s;
    }

    private String headerTitle; // [lcs] the header's title (null: the app's name)

    void setHeaderTitle(String s) {
        headerTitle = s;
    }

    int selectedCategory() { return cat; }

    int selectedItem() { return cats.isEmpty() ? 0 : cats.get(cat).sel; }

    void select(int category, int item) {
        if (cats.isEmpty())
            return;
        cat = Math.max(0, Math.min(cats.size() - 1, category));
        catPos = cat;
        cats.get(cat).sel = Math.max(0, item);
        refresh();
        snapScroll();
    }

    void setInsets(int l, int t, int r, int b) {
        insetL = l;
        insetT = t;
        insetR = r;
        insetB = b;
        layout();
        invalidate();
    }

    /** Re-reads every item's value, help and state (the activity calls it after any change) */
    void refresh() {
        for (Category c : cats) {
            Item was = c.sel < c.shown.size() ? c.shown.get(c.sel) : null;
            c.shown.clear();
            for (Item i : c.items) {
                if (i.visible != null && !i.visible.getAsBoolean())
                    continue;
                i.valueText = i.value != null ? nz(i.value.get()) : "";
                i.helpText = i.help != null ? nz(i.help.get()) : "";
                i.pictureFile = i.picture != null ? i.picture.get() : null;
                i.on = i.enabled == null || i.enabled.getAsBoolean();
                c.shown.add(i);
            }
            int keep = was != null ? c.shown.indexOf(was) : -1;
            c.sel = keep >= 0 ? keep : Math.max(0, Math.min(c.sel, c.shown.size() - 1));
        }
        if (sub != null && !cats.get(cat).shown.contains(sub))
            sub = null;
        invalidate();
    }

    private static String nz(String s) { return s == null ? "" : s; }

    // ---- layout ----------------------------------------------------------------------------------------------------

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        layout();
        snapScroll();
    }

    private void layout() {
        float W = getWidth(), H = getHeight();
        if (W == 0 || H == 0)
            return;
        float density = getResources().getDisplayMetrics().density;
        left = insetL;
        top = insetT;
        right = W - insetR;
        bottom = H - insetB;
        float sw = right - left, sh = bottom - top;
        wide = sw > sh * 1.25f;
        // the text unit: from the area (a 1080x2420 cover screen ~57 px, a 2352x2172 inner one ~76 px), bounded by
        // dp so it stays readable, and by the height so a short landscape window still shows a few rows
        T = clamp((float) Math.sqrt(sw * sh) * 0.038f, 17 * density, 30 * density);
        T = Math.max(15 * density, Math.min(T, sh * 0.056f));
        // a landscape window is short: a tighter bar and rows, so four or more rows fit on a 1080 px tall screen
        icon = T * (wide ? 1.7f : 2.1f);
        spacing = icon * (wide ? 2.3f : 1.7f);
        colX = left + Math.max(T * 1.2f, sw * (wide ? 0.12f : 0.06f)) + icon * 0.5f;
        barY = top + T * (wide ? 1.9f : 2.3f) + icon * 0.5f;
        catLabelY = barY + icon * 0.5f + T * (wide ? 1.15f : 1.3f);
        listTop = catLabelY + T * (wide ? 0.6f : 0.9f);
        rowH = T * (wide ? 2.3f : 2.55f);
        textX = colX + icon * 0.5f;
        float hintH = T * 1.9f;
        // help beside the list on landscape windows and on big near-square ones (an unfolded foldable, a tablet):
        // there it fits as a column and the list keeps the screen's height
        helpBeside = sw > sh * 1.45f || (sw > sh * 0.95f && sw / T > 26);
        if (helpBeside) {
            helpX = left + sw * 0.62f;
            helpW = right - T * 1.4f - helpX;
            helpY = listTop + T * 0.4f;
            helpBottom = bottom - hintH;
            textRight = helpX - T * 1.2f;
            listBottom = bottom - hintH;
        } else {
            textRight = right - T * 1.2f;
            helpX = textX;
            helpW = textRight - helpX;
            // no help text under the list any more (2026-11-17: the explanations went, the rows' values stay); the
            // list takes the room
            float helpH = 0;
            listBottom = bottom - hintH - helpH;
            helpY = listBottom + T * 0.5f;
            helpBottom = bottom - hintH;
        }
        title.setTextSize(T);
        small.setTextSize(T * 0.74f);
        helpPaint.setTextSize(T * 0.8f);
        helpLayout = null;
    }

    private static float clamp(float v, float lo, float hi) {
        return Math.max(lo, Math.min(hi, v));
    }

    private int visibleRows() {
        float top = sub != null ? listTop + T * 1.5f : listTop;
        return Math.max(1, (int) ((listBottom - top) / rowH));
    }

    private int rowCount() {
        return sub != null ? sub.choices.length : cats.isEmpty() ? 0 : cats.get(cat).shown.size();
    }

    private int selRow() {
        return sub != null ? subSel : cats.isEmpty() ? 0 : cats.get(cat).sel;
    }

    private float maxScroll() {
        return Math.max(0, rowCount() - visibleRows());
    }

    /** Keeps the selected row in view with a row of context above and below when there is room */
    private void follow() {
        touchScrolled = false;
        flingV = 0;
        int n = visibleRows(), sel = selRow();
        float s = scrollTarget;
        if (n >= 3) {
            if (sel < s + 1)
                s = sel - 1;
            if (sel > s + n - 2)
                s = sel - n + 2;
        } else {
            s = sel;
        }
        scrollTarget = clamp(Math.round(s), 0, maxScroll());
        invalidate();
    }

    private void snapScroll() {
        scrollTarget = 0;
        follow();
        scroll = scrollTarget;
    }

    // ---- navigation ------------------------------------------------------------------------------------------------

    void setCategory(int c) {
        if (cats.isEmpty())
            return;
        c = Math.max(0, Math.min(cats.size() - 1, c));
        sub = null;
        if (c != cat) {
            cat = c;
            scroll = scrollTarget = 0;
        }
        follow();
        scroll = Math.min(scroll, maxScroll());
    }

    private void move(int dy) {
        int n = rowCount();
        if (n == 0)
            return;
        int s = Math.max(0, Math.min(n - 1, selRow() + dy));
        if (sub != null)
            subSel = s;
        else
            cats.get(cat).sel = s;
        follow();
    }

    private void confirm() {
        if (cats.isEmpty())
            return;
        if (sub != null) {
            Item it = sub;
            int pick = subSel;
            sub = null;
            scroll = scrollTarget = 0;
            follow();
            it.choose.accept(pick);
            return;
        }
        Category c = cats.get(cat);
        if (c.sel >= c.shown.size())
            return;
        Item it = c.shown.get(c.sel);
        if (!it.on) {
            flash = 1;
            invalidate();
            return;
        }
        if (it.choicesNow != null)
            it.choices = it.choicesNow.get();
        if (it.choices != null) {
            sub = it;
            int cur = it.current != null ? it.current.getAsInt() : 0;
            subCurrent = cur; // read once here: drawing never reads prefs or files
            subSel = Math.max(0, Math.min(it.choices.length - 1, cur));
            scrollTarget = 0;
            follow();
            scroll = scrollTarget;
        } else if (it.action != null) {
            it.action.run();
        }
    }

    /** Closes the open options; false when there is nothing to close (the activity's Back then leaves) */
    boolean back() {
        if (sub == null)
            return false;
        sub = null;
        scroll = scrollTarget = 0;
        follow();
        scroll = scrollTarget;
        return true;
    }

    boolean handleKey(KeyEvent e) {
        int code = e.getKeyCode();
        boolean ours;
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP: case KeyEvent.KEYCODE_DPAD_DOWN: case KeyEvent.KEYCODE_DPAD_LEFT:
            case KeyEvent.KEYCODE_DPAD_RIGHT: case KeyEvent.KEYCODE_DPAD_CENTER: case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: case KeyEvent.KEYCODE_SPACE: case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_ESCAPE: case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_R1: case KeyEvent.KEYCODE_PAGE_UP: case KeyEvent.KEYCODE_PAGE_DOWN:
            case KeyEvent.KEYCODE_BUTTON_START: case KeyEvent.KEYCODE_TAB:
                ours = true;
                break;
            case KeyEvent.KEYCODE_BUTTON_Y: // [lcs] the game switch
                ours = gameNames != null;
                break;
            case KeyEvent.KEYCODE_BUTTON_X: case KeyEvent.KEYCODE_INFO: // the row's explanation
                ours = true;
                break;
            default:
                ours = false;
        }
        if (!ours)
            return false;
        if (e.getAction() != KeyEvent.ACTION_DOWN)
            return true;
        int src = e.getSource();
        gamepad = (src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (src & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD
                || (src & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD;
        boolean first = e.getRepeatCount() == 0;
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP: move(-1); break;
            case KeyEvent.KEYCODE_DPAD_DOWN: move(1); break;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                if (sub != null) { if (first) back(); } else setCategory(cat - 1);
                break;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                if (sub == null) setCategory(cat + 1);
                break;
            case KeyEvent.KEYCODE_BUTTON_L1: case KeyEvent.KEYCODE_PAGE_UP:
                setCategory(cat - 1);
                break;
            case KeyEvent.KEYCODE_BUTTON_R1: case KeyEvent.KEYCODE_PAGE_DOWN: case KeyEvent.KEYCODE_TAB:
                setCategory(e.isShiftPressed() ? cat - 1 : cat + 1);
                break;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_ESCAPE:
                if (first) back(); // at the top level B does nothing (Back leaves the app)
                break;
            case KeyEvent.KEYCODE_BUTTON_START:
                if (first) host.play();
                break;
            case KeyEvent.KEYCODE_BUTTON_X: case KeyEvent.KEYCODE_INFO:
                if (first)
                    explain(focusedItem());
                break;
            case KeyEvent.KEYCODE_BUTTON_Y:
                if (first && sub == null) host.pickGame((gameSel + 1) % gameNames.length);
                break;
            default:
                if (first) confirm();
        }
        invalidate();
        return true;
    }

    // ---- touch -----------------------------------------------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                gamepad = false;
                downX = e.getX();
                downY = e.getY();
                scrollAtDown = scroll;
                dragMode = 0;
                flingV = 0;
                if (velocity != null)
                    velocity.recycle();
                velocity = VelocityTracker.obtain();
                velocity.addMovement(e);
                longFired = false;
                removeCallbacks(longPress);
                postDelayed(longPress, ViewConfiguration.getLongPressTimeout());
                invalidate();
                return true;
            case MotionEvent.ACTION_MOVE: {
                if (velocity != null)
                    velocity.addMovement(e);
                float dx = e.getX() - downX, dy = e.getY() - downY;
                if (dragMode == 0 && Math.hypot(dx, dy) > slop) {
                    dragMode = Math.abs(dx) > Math.abs(dy) ? 1 : 2;
                    removeCallbacks(longPress);
                }
                if (dragMode == 1) {
                    catPos = clamp(cat - dx / spacing, -0.4f, cats.size() - 0.6f);
                    invalidate();
                } else if (dragMode == 2) {
                    touchScrolled = true;
                    scroll = clamp(scrollAtDown - dy / rowH, -0.5f, maxScroll() + 0.5f);
                    invalidate();
                }
                return true;
            }
            case MotionEvent.ACTION_UP: {
                removeCallbacks(longPress);
                float vx = 0, vy = 0;
                if (velocity != null) {
                    velocity.addMovement(e);
                    velocity.computeCurrentVelocity(1000);
                    vx = velocity.getXVelocity();
                    vy = velocity.getYVelocity();
                    velocity.recycle();
                    velocity = null;
                }
                float dx = e.getX() - downX;
                if (dragMode == 0) {
                    if (!longFired)
                        tap(e.getX(), e.getY());
                } else if (dragMode == 1) {
                    int to = Math.round(cat - dx / spacing);
                    if (to == cat && Math.abs(vx) > 900 * getResources().getDisplayMetrics().density / 2.75f)
                        to = vx < 0 ? cat + 1 : cat - 1;
                    setCategory(to);
                } else {
                    flingV = -vy / rowH;
                }
                dragMode = 0;
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_CANCEL:
                removeCallbacks(longPress);
                if (dragMode == 1)
                    setCategory(cat);
                dragMode = 0;
                invalidate();
                return true;
        }
        return super.onTouchEvent(e);
    }

    // long press on a row: its full explanation (the touch twin of X on a pad)
    private boolean longFired;
    private final Runnable longPress = () -> {
        if (dragMode != 0 || cats.isEmpty())
            return;
        Item it = itemAt(downX, downY);
        if (it == null)
            return;
        longFired = true;
        performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
        explain(it);
    };

    /** The row under a point (a top-level row, not an option), or null */
    private Item itemAt(float x, float y) {
        if (sub != null || y < listTop - T * 0.2f || y > listBottom || x < colX - icon || x > textRight + T)
            return null;
        int row = (int) Math.floor((y - listTop) / rowH + scroll);
        List<Item> shown = cats.get(cat).shown;
        return row >= 0 && row < shown.size() ? shown.get(row) : null;
    }

    private Item focusedItem() {
        if (cats.isEmpty())
            return null;
        if (sub != null)
            return sub;
        Category cg = cats.get(cat);
        return cg.sel < cg.shown.size() ? cg.shown.get(cg.sel) : null;
    }

    private void explain(Item it) {
        if (it == null)
            return;
        String text = it.explanation();
        if (!text.isEmpty())
            host.showHelp(it.title, text);
    }

    private void tap(float x, float y) {
        if (cats.isEmpty())
            return;
        if (tapGameSwitch(x, y)) // [lcs]
            return;
        if (y < listTop - T * 0.2f) {
            if (y > top + T * 1.6f) {
                for (int i = 0; i < cats.size(); i++)
                    if (Math.abs(x - barX(i)) < spacing * 0.5f) {
                        if (i != cat || sub != null)
                            setCategory(i);
                        return;
                    }
            }
            return;
        }
        if (sub != null && y < listTop + T * 1.5f) {
            back();
            return;
        }
        float rowsTop = sub != null ? listTop + T * 1.5f : listTop;
        if (y > listBottom || x < colX - icon || x > textRight + T)
            return;
        int row = (int) Math.floor((y - rowsTop) / rowH + scroll);
        if (row < 0 || row >= rowCount())
            return;
        if (sub == null && row == cats.get(cat).sel && x > textRight - T * 1.8f && cats.get(cat).shown.get(row).hasInfo()) {
            explain(cats.get(cat).shown.get(row)); // the selected row's "i"
            return;
        }
        if (sub != null)
            subSel = row;
        else
            cats.get(cat).sel = row;
        confirm();
        invalidate();
    }

    // ---- drawing ---------------------------------------------------------------------------------------------------

    @Override
    protected void onWindowVisibilityChanged(int v) {
        super.onWindowVisibilityChanged(v);
        removeCallbacks(clock);
        if (v == VISIBLE)
            postDelayed(clock, 20_000);
    }

    private static int col(float a, int rgb) {
        return (Math.max(0, Math.min(255, Math.round(255 * a))) << 24) | (rgb & 0xFFFFFF);
    }

    @Override
    protected void onDraw(Canvas c) {
        if (cats.isEmpty() || T == 0)
            return;
        long now = SystemClock.uptimeMillis();
        float dt = lastFrame == 0 ? 0.016f : Math.min(0.05f, (now - lastFrame) / 1000f);
        lastFrame = now;
        float k = 1 - (float) Math.exp(-dt * 14);
        boolean moving = false;
        if (dragMode != 1) {
            float d = cat - catPos;
            if (Math.abs(d) < 0.002f)
                catPos = cat;
            else {
                catPos += d * k;
                moving = true;
            }
        }
        if (dragMode != 2) {
            if (touchScrolled) {
                if (flingV != 0) {
                    scroll += flingV * dt;
                    flingV *= (float) Math.exp(-dt * 3.5f);
                    if (Math.abs(flingV) < 0.05f)
                        flingV = 0;
                    moving = true;
                }
                float lim = clamp(scroll, 0, maxScroll());
                if (Math.abs(lim - scroll) > 0.002f) {
                    scroll += (lim - scroll) * k;
                    if (flingV != 0 && (scroll < 0 || scroll > maxScroll()))
                        flingV *= 0.5f;
                    moving = true;
                } else if (flingV == 0) {
                    scroll = lim;
                }
            } else {
                float d = scrollTarget - scroll;
                if (Math.abs(d) < 0.002f)
                    scroll = scrollTarget;
                else {
                    scroll += d * k;
                    moving = true;
                }
            }
        }
        if (flash > 0) {
            flash = Math.max(0, flash - dt * 3);
            moving = true;
        }

        drawHeader(c);
        drawBar(c);
        drawList(c);
        drawHelp(c);
        drawHints(c);

        if (moving)
            postInvalidateOnAnimation();
        else
            lastFrame = 0;
    }

    private void drawHeader(Canvas c) {
        float y = top + T * 1.35f;
        String name = headerTitle != null ? headerTitle : getResources().getString(R.string.app_name);
        String time = android.text.format.DateFormat.getTimeFormat(getContext()).format(new Date());
        small.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        float titleW = small.measureText(name);
        small.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        float timeW = small.measureText(time);
        boolean showTitle = true;
        if (gameNames != null) {
            // [lcs] the game switch after the title: full names when they fit, else short ones, else in the title's place
            float from = left + T * 1.2f + titleW + T * 0.9f, to = right - T * 1.2f - timeW - T * 0.9f;
            String[] labels = gameNames;
            if (switchWidth(labels) > to - from)
                labels = gameShort;
            if (switchWidth(labels) > to - from) {
                showTitle = false;
                from = left + T * 1.2f;
            }
            drawGameSwitch(c, labels, from, y);
        } else if (gameRects != null) {
            gameRects = null;
        }
        small.setColor(col(0.9f, Palette.INK));
        small.setTextAlign(Paint.Align.LEFT);
        if (showTitle) {
            small.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            c.drawText(name, left + T * 1.2f, y, small);
            small.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        }
        small.setTextAlign(Paint.Align.RIGHT);
        c.drawText(time, right - T * 1.2f, y, small);
        small.setTextAlign(Paint.Align.LEFT);
    }

    // ---- [lcs] the game switch (Vice City Stories | Liberty City Stories), as the desktop launcher's title bar has:
    // a tap on a side, or Y on a gamepad, picks the game; the host swaps the palette (Palette.fadeTo) and the rows ----

    private String[] gameNames, gameShort;
    private int gameSel;
    private boolean gameSwitchOn;
    private RectF[] gameRects; // the segments as last drawn, for taps

    /** names / shortNames: the games (null: no switch); on: can switch now (not while a game runs) */
    void setGameSwitch(String[] names, String[] shortNames, int selected, boolean on) {
        gameNames = names;
        gameShort = shortNames;
        gameSel = selected;
        gameSwitchOn = on;
        invalidate();
    }

    private float switchWidth(String[] labels) {
        float w = 0;
        for (String s : labels)
            w += small.measureText(s) + T * 1.3f;
        return w;
    }

    private void drawGameSwitch(Canvas c, String[] labels, float x, float baseline) {
        float h = T * 1.25f, cy = baseline - T * 0.27f, a = gameSwitchOn ? 1f : 0.45f;
        float total = switchWidth(labels);
        if (gameRects == null || gameRects.length != labels.length) {
            gameRects = new RectF[labels.length];
            for (int i = 0; i < labels.length; i++)
                gameRects[i] = new RectF();
        }
        rect.set(x, cy - h / 2, x + total, cy + h / 2);
        fill.setColor(col(0.62f * a, Palette.GLASS));
        c.drawRoundRect(rect, h / 2, h / 2, fill);
        stroke.setStrokeWidth(Math.max(1.5f, T * 0.05f));
        stroke.setColor(col(0.55f * a, Palette.TEAL));
        c.drawRoundRect(rect, h / 2, h / 2, stroke);
        small.setTextAlign(Paint.Align.CENTER);
        float sx = x;
        for (int i = 0; i < labels.length; i++) {
            float w = small.measureText(labels[i]) + T * 1.3f;
            gameRects[i].set(sx, cy - h / 2 - T * 0.3f, sx + w, cy + h / 2 + T * 0.3f); // a little taller to tap
            boolean sel = i == gameSel;
            if (sel) {
                rect.set(sx + T * 0.12f, cy - h / 2 + T * 0.12f, sx + w - T * 0.12f, cy + h / 2 - T * 0.12f);
                fill.setColor(col(a, Palette.PINK)); // the primary accent, its text in the glass colour (>= 6:1)
                c.drawRoundRect(rect, h / 2, h / 2, fill);
            }
            small.setTypeface(Typeface.create(sel ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
            small.setColor(sel ? col(a, Palette.GLASS) : col(0.8f * a, Palette.INK));
            c.drawText(labels[i], sx + w / 2, baseline, small);
            sx += w;
        }
        small.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        small.setTextAlign(Paint.Align.LEFT);
    }

    /** A tap on the switch: true when it took it */
    private boolean tapGameSwitch(float x, float y) {
        if (gameNames == null || gameRects == null)
            return false;
        for (int i = 0; i < gameRects.length; i++)
            if (gameRects[i].contains(x, y)) {
                if (i != gameSel)
                    host.pickGame(i);
                return true;
            }
        return false;
    }

    /** A category glyph's x: the bar scrolls with the selection only until its last glyph is on screen, so a wide
     *  window shows every category instead of pushing the earlier ones off the left edge. */
    private float barX(float i) {
        float fit = (right - T * 1.2f - icon * 0.5f - colX) / spacing;
        float shift = clamp(catPos, 0, Math.max(0, cats.size() - 1 - fit));
        return colX + (i - shift) * spacing;
    }

    private void drawBar(Canvas c) {
        float dim = sub != null ? 0.45f : 1f;
        for (int i = 0; i < cats.size(); i++) {
            float x = barX(i);
            if (x < -icon || x > getWidth() + icon)
                continue;
            float d = Math.min(1, Math.abs(i - catPos));
            float s = icon * (1 - d * 0.28f);
            float a = (1 - d * 0.5f) * dim;
            if (d < 0.5f) {
                float r = icon * 0.95f;
                glowPaint.setShader(new RadialGradient(x, barY, r, col(0.35f * (1 - d * 2) * dim, Palette.PINK), 0, Shader.TileMode.CLAMP));
                c.drawCircle(x, barY, r, glowPaint);
            }
            drawGlyph(c, cats.get(i).glyph, x, barY, s, col(a, Palette.INK));
        }
        // the selected category's name under its glyph
        String name = cats.get(cat).name;
        title.setTextSize(T * 0.95f);
        title.setColor(col(dim * (1 - Math.min(1, Math.abs(cat - catPos)) * 0.7f), Palette.INK));
        float w = title.measureText(name);
        float x = clamp(barX(catPos) - w / 2, left + T * 0.6f, right - T * 0.6f - w);
        c.drawText(name, x, catLabelY, title);
        title.setTextSize(T);
    }

    private void drawList(Canvas c) {
        Category cg = cats.get(cat);
        float fade = 1 - Math.min(1, Math.abs(cat - catPos)) * 0.85f;
        float shift = (cat - catPos) * T * 3;
        c.save();
        c.clipRect(0, listTop - T * 0.2f, getWidth(), listBottom + T * 0.2f);
        float rowsTop = listTop;
        if (sub != null) {
            // the options' heading: the choice's name, tap (or Back) to close
            small.setColor(col(0.95f, Palette.PINK));
            String head = "‹  " + sub.title;
            c.drawText(TextUtils.ellipsize(head, small, textRight - (colX - icon * 0.5f), TextUtils.TruncateAt.END).toString(),
                    colX - icon * 0.5f, listTop + T * 0.95f, small);
            rowsTop = listTop + T * 1.5f;
        }
        int n = rowCount(), sel = selRow();
        int first = Math.max(0, (int) Math.floor(scroll) - 1);
        for (int r = first; r < n; r++) {
            float y = rowsTop + (r - scroll) * rowH;
            if (y > listBottom + rowH)
                break;
            if (y < rowsTop - rowH * 1.5f)
                continue;
            boolean selected = r == sel;
            if (sub != null)
                drawOption(c, r, y, selected);
            else
                drawItem(c, cg.shown.get(r), y + 0, selected, fade, shift);
        }
        c.restore();
        // a hint that there is more above / below
        if (scroll > 0.05f)
            drawChevron(c, colX, rowsTop - T * 0.05f, -1);
        if (scroll < maxScroll() - 0.05f)
            drawChevron(c, colX, listBottom + T * 0.1f, 1);
    }

    private void drawChevron(Canvas c, float x, float y, int dir) {
        stroke.setStrokeWidth(T * 0.09f);
        stroke.setColor(col(0.55f, Palette.INK));
        float s = T * 0.28f;
        path.rewind();
        path.moveTo(x - s, y - dir * s * 0.5f);
        path.lineTo(x, y + dir * s * 0.5f);
        path.lineTo(x + s, y - dir * s * 0.5f);
        c.drawPath(path, stroke);
    }

    private void highlight(Canvas c, float y, float a) {
        rect.set(colX - icon * 0.62f, y + T * 0.12f, textRight + T * 0.5f, y + rowH - T * 0.12f);
        fill.setColor(col(0.62f * a, Palette.GLASS));
        c.drawRoundRect(rect, T * 0.5f, T * 0.5f, fill);
        stroke.setStrokeWidth(Math.max(1.5f, T * 0.05f));
        stroke.setColor(col(0.55f * a, Palette.TEAL));
        c.drawRoundRect(rect, T * 0.5f, T * 0.5f, stroke);
        if (flash > 0) {
            fill.setColor(col(0.35f * flash, Palette.PINK));
            c.drawRoundRect(rect, T * 0.5f, T * 0.5f, fill);
        }
    }

    private void drawItem(Canvas c, Item it, float y, boolean selected, float fade, float shift) {
        c.save();
        c.translate(shift, 0);
        float a = fade * (it.on ? 1f : 0.42f);
        if (selected)
            highlight(c, y, fade);
        // the row's marker in the category's column
        float my = y + rowH * 0.5f;
        if (selected) {
            fill.setColor(col(fade, it.primary ? Palette.PINK : Palette.TEAL));
            c.drawCircle(colX, my, T * 0.2f, fill);
        } else {
            stroke.setStrokeWidth(Math.max(1.5f, T * 0.06f));
            stroke.setColor(col(0.6f * a, Palette.INK));
            c.drawCircle(colX, my, T * 0.16f, stroke);
        }
        boolean info = selected && it.hasInfo();
        float avail = textRight - textX - (info ? T * 1.6f : 0);
        boolean big = it.primary;
        title.setTextSize(big ? T * 1.15f : T);
        title.setTypeface(Typeface.create(big ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        title.setColor(col(a, big && it.on ? Palette.PINK : Palette.INK));
        String t = TextUtils.ellipsize(it.title, title, avail, TextUtils.TruncateAt.END).toString();
        boolean twoLines = !it.valueText.isEmpty();
        // the selected row's value wraps onto a second line instead of ending in "..."
        String v = it.valueText, v2 = null;
        if (twoLines && selected && small.measureText(v) > avail) {
            int n = small.breakText(v, true, avail, null);
            int sp = v.lastIndexOf(' ', n);
            if (sp > 0 && n < v.length())
                n = sp;
            v2 = v.substring(n).trim();
            v = v.substring(0, n).trim();
        }
        float ty = v2 != null ? y + rowH * 0.33f + T * 0.3f : twoLines ? y + rowH * 0.47f : y + rowH * 0.5f + T * 0.36f;
        c.drawText(t, textX, ty, title);
        title.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        title.setTextSize(T);
        if (twoLines) {
            int tone = v.startsWith("✓") ? Palette.TEAL : v.startsWith("✗") ? Palette.BAD : 0;
            small.setColor(col(a * (tone != 0 ? 1f : 0.7f), tone != 0 ? tone : Palette.INK));
            if (tone == 0 && it.choices != null)
                small.setColor(col(a * 0.95f, Palette.TEAL));
            float vy = v2 != null ? y + rowH * 0.65f : y + rowH * 0.47f + T * 0.98f;
            c.drawText(TextUtils.ellipsize(v, small, avail, TextUtils.TruncateAt.END).toString(), textX, vy, small);
            if (v2 != null)
                c.drawText(TextUtils.ellipsize(v2, small, avail, TextUtils.TruncateAt.END).toString(), textX, vy + T * 0.9f, small);
        }
        if (info) { // a small "i": tap it, or long-press the row, or X on a pad
            float ix = textRight - T * 0.7f, r = T * 0.42f;
            stroke.setStrokeWidth(Math.max(1.5f, T * 0.05f));
            stroke.setColor(col(0.75f * a, Palette.TEAL));
            c.drawCircle(ix, my, r, stroke);
            small.setColor(col(0.9f * a, Palette.TEAL));
            small.setTextAlign(Paint.Align.CENTER);
            c.drawText("i", ix, my + T * 0.26f, small);
            small.setTextAlign(Paint.Align.LEFT);
        }
        c.restore();
    }

    private void drawOption(Canvas c, int r, float y, boolean selected) {
        if (selected)
            highlight(c, y, 1);
        boolean current = subCurrent == r;
        float my = y + rowH * 0.5f;
        if (current) {
            fill.setColor(col(1, Palette.TEAL));
            c.drawCircle(colX, my, T * 0.22f, fill);
        } else {
            stroke.setStrokeWidth(Math.max(1.5f, T * 0.06f));
            stroke.setColor(col(0.6f, Palette.INK));
            c.drawCircle(colX, my, T * 0.16f, stroke);
        }
        title.setColor(col(1, current ? Palette.TEAL : Palette.INK));
        c.drawText(TextUtils.ellipsize(sub.choices[r], title, textRight - textX, TextUtils.TruncateAt.END).toString(),
                textX, my + T * 0.36f, title);
    }

    // the focused item's picture, decoded once per file and change (scaled down to about the help column's width)
    private Bitmap picture;
    private String pictureKey;

    private Bitmap picture(File f, int width) {
        String key = f.getPath() + "@" + f.lastModified() + "/" + width;
        if (key.equals(pictureKey))
            return picture;
        pictureKey = key;
        picture = null;
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(f.getPath(), o);
        if (o.outWidth <= 0)
            return null;
        o.inJustDecodeBounds = false;
        o.inSampleSize = 1;
        while (o.outWidth / (o.inSampleSize * 2) >= width)
            o.inSampleSize *= 2;
        picture = BitmapFactory.decodeFile(f.getPath(), o);
        return picture;
    }

    private void drawHelp(Canvas c) {
        Category cg = cats.get(cat);
        Item it = sub != null ? sub : cg.sel < cg.shown.size() ? cg.shown.get(cg.sel) : null;
        // only a picture (a save's) is shown: the help texts are no longer drawn (the user found them noise)
        String text = "";
        if (helpW < T * 4)
            return;
        float fade = 1 - Math.min(1, Math.abs(cat - catPos));
        // a picture first (a save's): the help column's width, at most 60 % of its height
        float picH = 0;
        Bitmap bm = it != null && sub == null && it.pictureFile != null ? picture(it.pictureFile, (int) helpW) : null;
        if (bm != null && bm.getWidth() > 0) {
            float wPic = helpW, hPic = wPic * bm.getHeight() / bm.getWidth(), most = (helpBottom - helpY) * 0.6f;
            if (hPic > most) {
                hPic = most;
                wPic = hPic * bm.getWidth() / bm.getHeight();
            }
            if (hPic >= T * 3) {
                RectF dst = new RectF(helpX, helpY, helpX + wPic, helpY + hPic);
                Paint pp = new Paint(Paint.FILTER_BITMAP_FLAG);
                pp.setAlpha((int) (255 * fade));
                c.drawBitmap(bm, null, dst, pp);
                picH = hPic + T * 0.6f;
            }
        }
        if (text.isEmpty())
            return;
        int width = (int) helpW;
        String layoutKey = text + "\u0000" + picH;
        if (helpLayout == null || !layoutKey.equals(helpLayoutText) || helpLayoutWidth != width) {
            helpPaint.setColor(col(0.78f, Palette.INK));
            int maxLines = Math.max(1, (int) ((helpBottom - helpY - picH) / (helpPaint.getTextSize() * 1.3f)));
            helpLayout = StaticLayout.Builder.obtain(text, 0, text.length(), helpPaint, width)
                    .setAlignment(Layout.Alignment.ALIGN_NORMAL)
                    .setLineSpacing(0, 1.12f)
                    .setMaxLines(maxLines)
                    .setEllipsize(TextUtils.TruncateAt.END)
                    .build();
            helpLayoutText = layoutKey;
            helpLayoutWidth = width;
        }
        c.save();
        c.translate(helpX, helpY + picH);
        if (helpBeside) {
            // a thin teal rule beside the text, as the overlay's panels have
            fill.setColor(col(0.6f * fade, Palette.TEAL));
            c.drawRect(-T * 0.6f, 0, -T * 0.6f + Math.max(2, T * 0.06f), Math.min(helpLayout.getHeight(), helpBottom - helpY - picH), fill);
        }
        helpPaint.setColor(col(0.8f * fade, Palette.INK));
        helpLayout.draw(c);
        c.restore();
    }

    private void drawHints(Canvas c) {
        float y = bottom - T * 0.75f;
        float x = left + T * 1.2f;
        small.setTextSize(T * 0.66f);
        if (gamepad) {
            x = hint(c, x, y, "A", sub != null ? "Choose" : "Select");
            x = hint(c, x, y, "B", "Back");
            x = hint(c, x, y, "X", "Info");
            x = hint(c, x, y, "L1 R1", "Category");
            x = hint(c, x, y, "≡", "Play");
            if (gameNames != null)
                x = hint(c, x, y, "Y", "Game"); // [lcs]
        } // touch: no hint line (the user's call, 2026-11-17); tapping and swiping are learnt in a moment
        // the build tag, faint, at the right when there is room
        small.setTextSize(T * 0.55f);
        small.setColor(col(0.4f, Palette.INK));
        float w = small.measureText(buildTag);
        if (right - T * 1.2f - w > x)
            c.drawText(buildTag, right - T * 1.2f - w, y, small);
        small.setTextSize(T * 0.74f);
    }

    /** A button cap (our own round key) and its action */
    private float hint(Canvas c, float x, float y, String key, String label) {
        float h = T * 0.95f;
        float kw = Math.max(h, small.measureText(key) + T * 0.6f);
        rect.set(x, y - h * 0.78f, x + kw, y + h * 0.22f);
        stroke.setStrokeWidth(Math.max(1.5f, T * 0.05f));
        stroke.setColor(col(0.75f, Palette.TEAL));
        c.drawRoundRect(rect, h / 2, h / 2, stroke);
        small.setColor(col(0.9f, Palette.INK));
        small.setTextAlign(Paint.Align.CENTER);
        c.drawText(key, x + kw / 2, y - h * 0.08f, small);
        small.setTextAlign(Paint.Align.LEFT);
        small.setColor(col(0.65f, Palette.INK));
        c.drawText(label, x + kw + T * 0.35f, y - h * 0.08f, small);
        return x + kw + T * 0.35f + small.measureText(label) + T * 1.1f;
    }

    // ---- the category glyphs: simple line drawings of our own ----

    private void drawGlyph(Canvas c, int g, float x, float y, float s, int color) {
        stroke.setColor(color);
        fill.setColor(color);
        stroke.setStrokeWidth(Math.max(2, s * 0.07f));
        float r = s * 0.42f;
        switch (g) {
            case G_PLAY:
                c.drawCircle(x, y, r, stroke);
                path.rewind();
                path.moveTo(x - r * 0.32f, y - r * 0.45f);
                path.lineTo(x + r * 0.52f, y);
                path.lineTo(x - r * 0.32f, y + r * 0.45f);
                path.close();
                c.drawPath(path, fill);
                break;
            case G_DISC:
                c.drawCircle(x, y, r, stroke);
                c.drawCircle(x, y, r * 0.24f, stroke);
                rect.set(x - r * 0.66f, y - r * 0.66f, x + r * 0.66f, y + r * 0.66f);
                c.drawArc(rect, 200, 70, false, stroke);
                break;
            case G_SETTINGS: {
                float w = s * 0.42f;
                for (int i = -1; i <= 1; i++) {
                    float ly = y + i * s * 0.24f;
                    c.drawLine(x - w, ly, x + w, ly, stroke);
                    float kx = x + w * (i == 0 ? 0.35f : i < 0 ? -0.45f : 0.1f);
                    c.drawCircle(kx, ly, s * 0.085f, fill);
                }
                break;
            }
            case G_CONTROLS: {
                rect.set(x - s * 0.46f, y - s * 0.26f, x + s * 0.46f, y + s * 0.26f);
                c.drawRoundRect(rect, s * 0.24f, s * 0.24f, stroke);
                float px = x - s * 0.22f, q = s * 0.1f;
                c.drawLine(px - q, y, px + q, y, stroke);
                c.drawLine(px, y - q, px, y + q, stroke);
                c.drawCircle(x + s * 0.18f, y + s * 0.05f, s * 0.05f, fill);
                c.drawCircle(x + s * 0.29f, y - s * 0.07f, s * 0.05f, fill);
                break;
            }
            case G_FRAMEGEN: {
                rect.set(x - s * 0.44f, y - s * 0.3f, x + s * 0.14f, y + s * 0.16f);
                int dimmed = col(((color >>> 24) / 255f) * 0.5f, color);
                stroke.setColor(dimmed);
                c.drawRoundRect(rect, s * 0.06f, s * 0.06f, stroke);
                stroke.setColor(color);
                rect.set(x - s * 0.18f, y - s * 0.12f, x + s * 0.44f, y + s * 0.34f);
                c.drawRoundRect(rect, s * 0.06f, s * 0.06f, stroke);
                c.drawLine(x - s * 0.47f, y + s * 0.3f, x - s * 0.3f, y + s * 0.3f, stroke);
                c.drawLine(x - s * 0.47f, y + s * 0.42f, x - s * 0.22f, y + s * 0.42f, stroke);
                break;
            }
            case G_SAVES: { // [saves] a memory card: a cut corner, a label, the contacts
                float w = s * 0.3f, h = s * 0.42f, cut = s * 0.14f;
                path.rewind();
                path.moveTo(x - w, y - h);
                path.lineTo(x + w - cut, y - h);
                path.lineTo(x + w, y - h + cut);
                path.lineTo(x + w, y + h);
                path.lineTo(x - w, y + h);
                path.close();
                c.drawPath(path, stroke);
                rect.set(x - w * 0.6f, y - h * 0.55f, x + w * 0.45f, y - h * 0.05f);
                c.drawRect(rect, stroke);
                for (int i = 0; i < 4; i++)
                    c.drawLine(x - w * 0.6f + i * w * 0.38f, y + h * 0.45f, x - w * 0.6f + i * w * 0.38f, y + h * 0.75f, stroke);
                break;
            }
            default: // about
                c.drawCircle(x, y, r, stroke);
                c.drawCircle(x, y - r * 0.42f, s * 0.05f, fill);
                c.drawLine(x, y - r * 0.08f, x, y + r * 0.5f, stroke);
        }
    }
}
