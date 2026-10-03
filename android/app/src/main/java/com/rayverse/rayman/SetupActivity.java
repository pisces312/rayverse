package com.rayverse.rayman;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.TextUtils;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.List;

/**
 * Setup screen for choosing game data directory via SAF.
 * Once data is validated, launches RayverseActivity (SDL).
 */
public class SetupActivity extends Activity {
    private static final String TAG = "Rayverse-Setup";
    private static final int REQUEST_OPEN_DOCUMENT_TREE = 1001;
    private static final int REQUEST_EXPORT_TREE = 1002;
    private static final int REQUEST_IMPORT_TREE = 1003;

    private GameDataBridge dataBridge;
    private TextView statusText;
    private Button selectButton;
    private Button startButton;
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

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 48, 48, 48);
        root.setBackgroundColor(0xFF1A1A2E);

        TextView title = new TextView(this);
        title.setText(getString(R.string.app_name));
        title.setTextSize(24);
        title.setTextColor(0xFFE94560);
        title.setPadding(0, 0, 0, 24);
        root.addView(title);

        statusText = new TextView(this);
        statusText.setTextSize(14);
        statusText.setTextColor(0xFFCCCCCC);
        statusText.setPadding(0, 0, 0, 24);
        root.addView(statusText);
        refreshSaveStatus();

        Button play = new Button(this);
        play.setText("Play");
        play.setOnClickListener(v -> launchGame());
        root.addView(play);

        Button exportButton = new Button(this);
        exportButton.setText("Export saves");
        exportButton.setOnClickListener(v -> openTree(REQUEST_EXPORT_TREE));
        root.addView(exportButton);

        Button importButton = new Button(this);
        importButton.setText("Import saves");
        importButton.setOnClickListener(v -> openTree(REQUEST_IMPORT_TREE));
        root.addView(importButton);

        Button changeButton = new Button(this);
        changeButton.setText("Change game data folder");
        changeButton.setOnClickListener(v -> openDocumentTree());
        root.addView(changeButton);

        setContentView(root);
    }

    private void refreshSaveStatus() {
        if (statusText == null) return;
        List<String> saves = dataBridge.listSaveFiles();
        statusText.setText(saves.isEmpty()
                ? "No save files on this device yet."
                : "Save files on device (" + saves.size() + "): " + TextUtils.join(", ", saves));
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

    private void showSetupScreen() {
        onHome = false;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 48, 48, 48);
        root.setBackgroundColor(0xFF1A1A2E);

        TextView title = new TextView(this);
        title.setText("Rayverse - Rayman 1");
        title.setTextSize(24);
        title.setTextColor(0xFFE94560);
        title.setPadding(0, 0, 0, 32);
        root.addView(title);

        TextView instructions = new TextView(this);
        instructions.setText(
            "Select the directory containing your Rayman 1 game data.\n\n" +
            "Required:\n" +
            "  PCMAP/ (level data)\n" +
            "  RAY.LNG, SNDD8B.DAT, SNDH8B.DAT\n\n" +
            "Optional:\n" +
            "  INTRO.DAT, CONCLU.DAT (cutscenes)\n" +
            "  Music/ (CD audio tracks)"
        );
        instructions.setTextSize(14);
        instructions.setTextColor(0xFFCCCCCC);
        instructions.setPadding(0, 0, 0, 32);
        root.addView(instructions);

        statusText = new TextView(this);
        statusText.setTextSize(14);
        statusText.setTextColor(0xFFFFAA00);
        statusText.setPadding(0, 0, 0, 16);
        root.addView(statusText);

        selectButton = new Button(this);
        selectButton.setText("Select Game Data Directory");
        selectButton.setOnClickListener(v -> openDocumentTree());
        root.addView(selectButton);

        startButton = new Button(this);
        startButton.setText("Start Game");
        startButton.setEnabled(false);
        startButton.setVisibility(View.GONE);
        startButton.setOnClickListener(v -> launchGame());
        root.addView(startButton);

        ScrollView scroll = new ScrollView(this);
        TextView scanDetails = new TextView(this);
        scanDetails.setTextSize(12);
        scanDetails.setTextColor(0xFF888888);
        scanDetails.setPadding(0, 24, 0, 0);
        scanDetails.setId(R.id.scan_details);
        scanDetails.setTag("scanDetails");
        scroll.addView(scanDetails);
        root.addView(scroll);

        setContentView(root);
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
                /* Home menu has no select/start buttons; just refresh the menu. */
                if (result.isComplete()) {
                    showHomeMenu();
                } else {
                    showSetupScreen();
                    setStatus("Missing required files. Please select the correct directory.", false);
                    TextView scanDetails = findViewById(R.id.scan_details);
                    if (scanDetails != null) scanDetails.setText(result.getSummary());
                }
                return;
            }

            statusText.setText("Scanning...");
            selectButton.setEnabled(false);

            TextView scanDetails = findViewById(R.id.scan_details);
            if (scanDetails != null) {
                scanDetails.setText(result.getSummary());
            }

            if (result.isComplete()) {
                statusText.setText("Game data found! Ready to play.");
                statusText.setTextColor(0xFF00FF00);
                startButton.setEnabled(true);
                startButton.setVisibility(View.VISIBLE);
                selectButton.setText("Change Directory");
            } else {
                statusText.setText("Missing required files. Please select the correct directory.");
                statusText.setTextColor(0xFFFF4444);
            }
            selectButton.setEnabled(true);
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
