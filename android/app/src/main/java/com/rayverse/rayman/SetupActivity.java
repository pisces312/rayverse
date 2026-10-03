package com.rayverse.rayman;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.TextUtils;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.List;

/**
 * Setup / home screen.
 * Landscape-friendly: a centered vertical column of buttons, with all explanatory
 * text reduced to single short lines and the main hint pinned to the bottom.
 */
public class SetupActivity extends Activity {
    private static final String TAG = "Rayverse-Setup";
    private static final int REQUEST_OPEN_DOCUMENT_TREE = 1001;
    private static final int REQUEST_EXPORT_TREE = 1002;
    private static final int REQUEST_IMPORT_TREE = 1003;

    private GameDataBridge dataBridge;
    private TextView statusText;
    private TextView saveListText;
    private Button selectButton;
    private boolean gameLaunched = false;
    private boolean onHome = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        dataBridge = new GameDataBridge(this);

        /* Data already configured: show the home menu so saves can be exported or
         * imported before the engine reads them once the game boots. */
        if (dataBridge.hasSavedUri() && dataBridge.loadSavedUri()) {
            GameDataBridge.ScanResult result = dataBridge.scanAndStore();
            if (result.isComplete()) {
                showHomeMenu();
                return;
            }
        }

        showSetupScreen();
    }

    private void showHomeMenu() {
        onHome = true;
        saveListText = null;

        LinearLayout root = newRoot();

        root.addView(centeredText(getString(R.string.app_name), 18, 0xFFE94560));

        statusText = centeredText("", 12, 0xFFCCCCCC);
        root.addView(statusText);

        saveListText = centeredText("", 12, 0xFF888888);
        root.addView(saveListText);
        refreshSaveStatus();

        addCenteredButton(root, "Play", v -> launchGame());
        addCenteredButton(root, "Export saves", v -> openTree(REQUEST_EXPORT_TREE));
        addCenteredButton(root, "Import saves", v -> openTree(REQUEST_IMPORT_TREE));
        addCenteredButton(root, "Change data folder", v -> openDocumentTree());
        addCenteredButton(root, "Exit", v -> finish());

        root.addView(spacer());
        root.addView(centeredText(
                "Export / Import copies RAYMAN*.SAV to or from a folder you pick.",
                11, 0xFF888888));

        setContentView(root);
    }

    private void showSetupScreen() {
        onHome = false;
        saveListText = null;

        LinearLayout root = newRoot();

        root.addView(centeredText("Rayverse - Rayman 1", 18, 0xFFE94560));

        statusText = centeredText("", 12, 0xFFFFAA00);
        root.addView(statusText);

        selectButton = menuButton("Select folder", v -> openDocumentTree());
        addCenteredButton(root, selectButton);
        addCenteredButton(root, "Import saves", v -> openTree(REQUEST_IMPORT_TREE));
        addCenteredButton(root, "Export saves", v -> openTree(REQUEST_EXPORT_TREE));
        addCenteredButton(root, "Exit", v -> finish());

        root.addView(spacer());

        /* The one-line replacement for the old multi-line required/optional list. */
        root.addView(centeredText(
                "Select the folder containing your Rayman 1 game data.",
                12, 0xFFCCCCCC));

        TextView scanDetails = centeredText("", 11, 0xFF888888);
        scanDetails.setId(R.id.scan_details);
        scanDetails.setTag("scanDetails");
        root.addView(scanDetails);

        setContentView(root);
    }

    /* ── layout helpers ── */

    private LinearLayout newRoot() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(24, 16, 24, 16);
        root.setBackgroundColor(0xFF1A1A2E);
        return root;
    }

    private TextView centeredText(String text, float sp, int color) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        t.setGravity(Gravity.CENTER_HORIZONTAL);
        return t;
    }

    private Button menuButton(String label, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextSize(13);
        b.setSingleLine(true);
        b.setOnClickListener(listener);
        return b;
    }

    private void addCenteredButton(LinearLayout root, String label, View.OnClickListener listener) {
        addCenteredButton(root, menuButton(label, listener));
    }

    private void addCenteredButton(LinearLayout root, Button b) {
        int width = (int) (240 * getResources().getDisplayMetrics().density);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                width, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, 4, 0, 4);
        lp.gravity = Gravity.CENTER_HORIZONTAL;
        root.addView(b, lp);
    }

    private View spacer() {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        return v;
    }

    private String shortSummary(GameDataBridge.ScanResult r) {
        if (r.isComplete()) return "Game data OK (" + r.found.size() + " files).";
        return "Missing: " + TextUtils.join(", ", r.missingRequired);
    }

    private void refreshSaveStatus() {
        if (saveListText == null) return;
        List<String> saves = dataBridge.listSaveFiles();
        saveListText.setText(saves.isEmpty()
                ? "No saves on device yet."
                : "Saves on device: " + saves.size());
    }

    private void setStatus(String msg, boolean ok) {
        if (statusText == null) return;
        statusText.setText(msg);
        statusText.setTextColor(ok ? 0xFF00FF00 : 0xFFFFAA00);
    }

    private void openTree(int requestCode) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                      | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        startActivityForResult(intent, requestCode);
    }

    private void openDocumentTree() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                      | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                      | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_OPEN_DOCUMENT_TREE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQUEST_OPEN_DOCUMENT_TREE && resultCode == RESULT_OK) {
            Uri treeUri = data.getData();
            if (treeUri == null) {
                setStatus("Error: No directory selected", false);
                return;
            }

            dataBridge.saveTreeUri(treeUri);
            GameDataBridge.ScanResult result = dataBridge.scanAndStore();

            if (onHome) {
                /* Home menu has no select button; just refresh or fall back to setup. */
                if (result.isComplete()) {
                    showHomeMenu();
                } else {
                    showSetupScreen();
                    setStatus("Missing required files.", false);
                    TextView scanDetails = findViewById(R.id.scan_details);
                    if (scanDetails != null) scanDetails.setText(shortSummary(result));
                }
                return;
            }

            statusText.setText("Scanning...");
            selectButton.setEnabled(false);

            TextView scanDetails = findViewById(R.id.scan_details);
            if (scanDetails != null) {
                scanDetails.setText(shortSummary(result));
            }

            if (result.isComplete()) {
                /* Data is valid now: land on the home menu (Play / Export / Import)
                 * so saves can be managed without entering the game first. */
                showHomeMenu();
            } else {
                setStatus("Missing required files.", false);
                selectButton.setEnabled(true);
            }
        } else if (requestCode == REQUEST_EXPORT_TREE && resultCode == RESULT_OK && data != null && data.getData() != null) {
            int n = dataBridge.exportSavesTo(data.getData());
            setStatus(n < 0 ? "Export failed."
                    : n == 0 ? "No save files to export."
                    : "Exported " + n + " save file(s).", n > 0);
        } else if (requestCode == REQUEST_IMPORT_TREE && resultCode == RESULT_OK && data != null && data.getData() != null) {
            final Uri src = data.getData();
            new AlertDialog.Builder(this)
                    .setTitle("Import saves")
                    .setMessage("Overwrite the saves on this device with the save files in the chosen folder?")
                    .setPositiveButton("Import", (d, w) -> {
                        int n = dataBridge.importSavesFrom(src);
                        setStatus(n < 0 ? "Import failed." : "Imported " + n + " save file(s).", n > 0);
                        if (onHome) showHomeMenu();
                    })
                    .setNegativeButton("Cancel", null)
                    .show();
        }
    }

    private void launchGame() {
        gameLaunched = true;
        startActivity(new Intent(this, RayverseActivity.class));
    }

    @Override
    protected void onStop() {
        super.onStop();
        /* This screen exists only to hand off to the game, and it has no content view on
         * the auto-start path, so keeping it would show a black screen when the game ends.
         * Finish once the game is on top of us: doing it from onCreate collapses the task
         * and kills the activity we just started, and leaving it here lets ActivityManager
         * resurrect the task when the game's process exits. */
        if (gameLaunched) {
            finish();
        }
    }
}
