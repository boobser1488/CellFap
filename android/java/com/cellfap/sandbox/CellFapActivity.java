// The Android activity of CellFap: NativeActivity (the game is libcellfap.so) plus the few things that need Java:
// screen orientation, the on-screen keyboard, the system file picker / "save as" dialog and the full-screen mode.
// The native side calls the public methods below through JNI; the static native methods are registered by
// android/android_main.cpp (RegisterNatives) and only queue events for the game thread.
package com.cellfap.sandbox;

import android.app.NativeActivity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.media.AudioManager;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.DisplayCutout;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class CellFapActivity extends NativeActivity {
    static native void nativeText(String text);         // characters typed on the on-screen keyboard
    static native void nativeBackspace(int count);       // characters erased by the keyboard
    static native void nativeEnter();                    // the keyboard's "done" key
    static native void nativeFilePicked(String path);    // a file chosen in the picker, copied into the app's storage
    static native void nativeMessage(String text);       // a short message for the player
    static native void nativeImeInset(int pixels);       // height of the on-screen keyboard (0 = hidden)
    static native void nativeSafeInsets(int left, int top, int right, int bottom);  // area covered by the camera cutout

    private static final int REQ_OPEN = 1, REQ_SAVE = 2;
    private TextInput input;
    private String exportSource;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        input = new TextInput(this);
        addContentView(input, new ViewGroup.LayoutParams(1, 1));
        // The whole screen is the game's: under the camera cutout too, no letterbox strips at any edge.
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30 ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                                                                       : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
            getWindow().setNavigationBarDividerColor(Color.TRANSPARENT);
        }
        getWindow().setNavigationBarColor(Color.BLACK);
        getWindow().setStatusBarColor(Color.BLACK);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS);
        if (Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(false);
        // Keyboard height (API 30+) and the camera cutout's safe area: the game draws the dish under the cutout but
        // keeps its buttons out of it.
        getWindow().getDecorView().setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                if (Build.VERSION.SDK_INT >= 30) nativeImeInset(insets.getInsets(WindowInsets.Type.ime()).bottom);
                if (Build.VERSION.SDK_INT >= 28) {
                    DisplayCutout dc = insets.getDisplayCutout();
                    if (dc != null) nativeSafeInsets(dc.getSafeInsetLeft(), dc.getSafeInsetTop(), dc.getSafeInsetRight(), dc.getSafeInsetBottom());
                    else nativeSafeInsets(0, 0, 0, 0);
                }
                return v.onApplyWindowInsets(insets);
            }
        });
        hideSystemBars();
        handleIncoming(getIntent());
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        View decor = getWindow().getDecorView();
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                                        View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                                        View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    // ---- called from the game thread
    // By the sensor, not "user" modes: those obey the system rotation lock, and with auto-rotate off the game never
    // followed how the phone is held (not even turning a landscape screen upside down).
    public void setOrientation(final int mode) {
        final int o = mode == 1 ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                    : mode == 2 ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                    : ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                setRequestedOrientation(o);
            }
        });
    }

    // The key click Android plays for a tapped view: the only sound of the original Cell Lab (View.playSoundEffect).
    // With an explicit volume it plays even when the system "touch sounds" are off; the game has its own switch.
    private AudioManager audio;

    public void playClick(float volume) {
        if (audio == null) audio = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        if (audio != null) audio.playSoundEffect(AudioManager.FX_KEY_CLICK, volume);
    }

    public void showKeyboard(final boolean show) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm == null) return;
                if (show) {
                    input.requestFocus();
                    imm.restartInput(input);
                    imm.showSoftInput(input, 0);
                } else {
                    imm.hideSoftInputFromWindow(input.getWindowToken(), 0);
                    hideSystemBars();
                }
            }
        });
    }

    public void pickFile() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                try {
                    startActivityForResult(i, REQ_OPEN);
                } catch (ActivityNotFoundException e) {
                    nativeMessage("На устройстве нет выбора файлов");
                }
            }
        });
    }

    public void exportFile(final String path, final String name) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                exportSource = path;
                Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("application/octet-stream");
                i.putExtra(Intent.EXTRA_TITLE, name);
                try {
                    startActivityForResult(i, REQ_SAVE);
                } catch (ActivityNotFoundException e) {
                    nativeMessage("На устройстве нет диалога сохранения");
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        try {
            if (request == REQ_OPEN) {
                importUri(uri);
            } else if (request == REQ_SAVE && exportSource != null) {
                try (InputStream in = new FileInputStream(exportSource); OutputStream os = getContentResolver().openOutputStream(uri, "wt")) {
                    copy(in, os);
                }
                nativeMessage("Файл сохранён");
            }
        } catch (Exception e) {
            nativeMessage("Не удалось: " + e.getMessage());
        }
    }

    // ---- files opened with CellFap from other apps (a messenger, a file manager) and shared from the game
    private final java.util.ArrayList<String> pendingFiles = new java.util.ArrayList<>();
    private boolean nativeReady = false;

    // A file handed to the game: copied into files/import and passed on once the native side is listening.
    private void importUri(Uri uri) throws java.io.IOException {
        File dir = new File(getFilesDir(), "import");
        dir.mkdirs();
        File out = new File(dir, displayName(uri));
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream os = new FileOutputStream(out)) {
            copy(in, os);
        }
        if (nativeReady) nativeFilePicked(out.getAbsolutePath());
        else pendingFiles.add(out.getAbsolutePath());
    }

    private void handleIncoming(Intent intent) {
        if (intent == null) return;
        Uri uri = null;
        if (Intent.ACTION_VIEW.equals(intent.getAction())) uri = intent.getData();
        else if (Intent.ACTION_SEND.equals(intent.getAction())) uri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
        if (uri == null) return;
        try {
            importUri(uri);
        } catch (Exception e) {
            if (nativeReady) nativeMessage("Не удалось открыть файл: " + e.getMessage());
        }
        intent.setAction(Intent.ACTION_MAIN);  // handled: not again after a restart
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIncoming(intent);
    }

    // Called by the game thread once its natives are registered.
    public void onNativeReady() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                nativeReady = true;
                for (String p : pendingFiles) nativeFilePicked(p);
                pendingFiles.clear();
            }
        });
    }

    public void shareFile(final String path, final String name, final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    File out = new File(ShareProvider.shareDir(CellFapActivity.this), name.replace('/', '_'));
                    try (InputStream in = new FileInputStream(path); OutputStream os = new FileOutputStream(out)) {
                        copy(in, os);
                    }
                    Uri uri = ShareProvider.uriFor(out.getName());
                    Intent i = new Intent(Intent.ACTION_SEND);
                    i.setType("application/octet-stream");
                    i.putExtra(Intent.EXTRA_STREAM, uri);
                    i.putExtra(Intent.EXTRA_TEXT, text);
                    i.setClipData(ClipData.newRawUri(name, uri));
                    i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    Intent chooser = Intent.createChooser(i, "Поделиться");
                    chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    startActivity(chooser);
                } catch (Exception e) {
                    nativeMessage("Не удалось поделиться: " + e.getMessage());
                }
            }
        });
    }

    // Hands a file straight to Cell Lab / MinLab ("open with"). They take files only through VIEW, not the share sheet:
    // a genome goes to the main menu (it copies the file under the URI's last segment, so the name must end in
    // ".genome"), a dish to the play screen. Another app that opens such files is offered when the game is missing.
    public void openInApp(final String path, final String name, final boolean genome) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    File out = new File(ShareProvider.shareDir(CellFapActivity.this), name.replace('/', '_'));
                    try (InputStream in = new FileInputStream(path); OutputStream os = new FileOutputStream(out)) {
                        copy(in, os);
                    }
                    Uri uri = ShareProvider.uriFor(out.getName());
                    Intent view = new Intent(Intent.ACTION_VIEW);
                    view.setDataAndType(uri, "application/octet-stream");
                    view.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
                    Intent game = new Intent(view);
                    game.setClassName("com.acl.cell_lab", genome ? "com.acl.cell_lab.ui.main_menu.MainMenuActivity"
                                                                 : "com.acl.cell_lab.ui.play_menu.PlayActivity");
                    try {
                        startActivity(game);
                        return;
                    } catch (Exception notInstalled) {
                        // fall through to any app that views the file
                    }
                    Intent chooser = Intent.createChooser(view, "Открыть в…");
                    chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    startActivity(chooser);
                } catch (Exception e) {
                    nativeMessage("Не удалось открыть: " + e.getMessage());
                }
            }
        });
    }

    // Test runs from adb ("am start ... --es cellfap_test '--screen 2 --preset 3'"): the native side keeps only a few
    // harmless options (screen, preset, sheet tab, experiment, hand-off to Cell Lab).
    public String testArgs() {
        Intent i = getIntent();
        return i != null ? i.getStringExtra("cellfap_test") : null;
    }

    private String displayName(Uri uri) {
        String name = null;
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) name = c.getString(0);
        } catch (Exception ignored) {
        }
        if (name == null || name.isEmpty()) name = "file";
        return name.replace('/', '_').replace('\\', '_');
    }

    private static void copy(InputStream in, OutputStream out) throws java.io.IOException {
        if (in == null || out == null) throw new java.io.IOException("нет доступа к файлу");
        byte[] buf = new byte[65536];
        for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
    }

    // An invisible view that owns the keyboard connection. Text goes straight to the game (NativeActivity has no text
    // field of its own); a word the keyboard is still composing is replaced by its newest version as it changes.
    static final class TextInput extends View {
        TextInput(Context c) {
            super(c);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            info.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_ACTION_DONE;
            return new BaseInputConnection(this, false) {
                private String composing = "";

                private void replaceComposing(String text) {
                    int p = 0;
                    while (p < composing.length() && p < text.length() && composing.charAt(p) == text.charAt(p)) ++p;
                    if (composing.length() > p) nativeBackspace(composing.codePointCount(p, composing.length()));
                    if (text.length() > p) nativeText(text.substring(p));
                    composing = text;
                }

                @Override
                public boolean commitText(CharSequence text, int newCursorPosition) {
                    replaceComposing(text.toString());
                    composing = "";
                    return true;
                }

                @Override
                public boolean setComposingText(CharSequence text, int newCursorPosition) {
                    replaceComposing(text.toString());
                    return true;
                }

                @Override
                public boolean finishComposingText() {
                    composing = "";
                    return true;
                }

                @Override
                public boolean deleteSurroundingText(int before, int after) {
                    composing = "";  // what was composed is already in the game's text field
                    if (before > 0) nativeBackspace(before);
                    return true;
                }

                @Override
                public boolean performEditorAction(int action) {
                    nativeEnter();
                    return true;
                }
            };
        }
    }
}
