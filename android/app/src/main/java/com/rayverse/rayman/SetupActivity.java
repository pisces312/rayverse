package com.rayverse.rayman;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * Setup screen for choosing game data directory via SAF.
 * Once data is validated, launches RayverseActivity (SDL).
 */
public class SetupActivity extends Activity {
    private static final String TAG = "Rayverse-Setup";
    private static final int REQUEST_OPEN_DOCUMENT_TREE = 1001;

    private GameDataBridge dataBridge;
    private TextView statusText;
    private Button selectButton;
    private Button startButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        dataBridge = new GameDataBridge(this);

        /* If we already have a validated saved URI, go straight to game */
        if (dataBridge.hasSavedUri() && dataBridge.loadSavedUri()) {
            GameDataBridge.ScanResult result = dataBridge.scanAndStore();
            if (result.isComplete()) {
                launchGame();
                return;
            }
        }

        showSetupScreen();
    }

    private void showSetupScreen() {
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
                statusText.setText("Error: No directory selected");
                return;
            }

            statusText.setText("Scanning...");
            selectButton.setEnabled(false);

            dataBridge.saveTreeUri(treeUri);
            GameDataBridge.ScanResult result = dataBridge.scanAndStore();

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
        }
    }

    private void launchGame() {
        startActivity(new Intent(this, RayverseActivity.class));
        /* The auto-start path never builds a content view, so resuming this activity
         * after the game quits would show a black screen. Leave the task instead. */
        finish();
    }
}
