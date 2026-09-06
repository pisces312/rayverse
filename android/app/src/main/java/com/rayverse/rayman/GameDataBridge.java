package com.rayverse.rayman;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Manages SAF-based game data access for Rayverse.
 *
 * Two-phase design:
 * - Phase 1 (SetupActivity): pure Java SAF scanning, no native libs needed
 * - Phase 2 (RayverseActivity): register fds via JNI after SDL loads native libs
 */
public class GameDataBridge {
    private static final String TAG = "GameDataBridge";
    private static final String PREFS_NAME = "rayverse_prefs";
    private static final String KEY_TREE_URI = "tree_uri";

    private static final String[] REQUIRED_FILES = {
        "PCMAP/ALLFIX.DAT",
        "PCMAP/RAY1.WLD",
        "PCMAP/RAY2.WLD",
        "PCMAP/RAY3.WLD",
        "PCMAP/RAY4.WLD",
        "PCMAP/RAY5.WLD",
        "PCMAP/RAY6.WLD",
        "RAY.LNG",
        "SNDD8B.DAT",
        "SNDH8B.DAT",
    };

    private static final String[] OPTIONAL_FILES = {
        "VIGNET.DAT",
        "SNDVIG.DAT",
        "CONCLU.DAT",
        "INTRO.DAT",
        "PCMAP/BRAY.DAT",
    };

    private static final Set<String> ALL_KNOWN_PATHS = new HashSet<>();
    static {
        ALL_KNOWN_PATHS.addAll(Arrays.asList(REQUIRED_FILES));
        ALL_KNOWN_PATHS.addAll(Arrays.asList(OPTIONAL_FILES));
        for (int t = 2; t <= 20; t++) {
            ALL_KNOWN_PATHS.add(String.format("Music/rayman%02d.ogg", t));
        }
    }

    /* JNI native methods — only callable after libmain.so is loaded by SDL */
    public static native void nativeRegisterFd(String path, int fd, int mode);
    public static native void nativeSetSafUri(String uri);
    public static native void nativeSetGameDataPath(String path, int useSaf);
    public static native void nativeSetSaveDir(String path);

    private final Context context;
    private final ContentResolver resolver;
    private Uri treeUri;

    /* Stored scan result for deferred JNI registration */
    private List<FdEntry> pendingFds = new ArrayList<>();

    public GameDataBridge(Context context) {
        this.context = context;
        this.resolver = context.getContentResolver();
    }

    /* ── Phase 1: pure Java (SetupActivity) ── */

    public boolean hasSavedUri() {
        return getPrefs().getString(KEY_TREE_URI, null) != null;
    }

    public boolean loadSavedUri() {
        String uriStr = getPrefs().getString(KEY_TREE_URI, null);
        if (uriStr == null) return false;
        treeUri = Uri.parse(uriStr);
        return true;
    }

    public void saveTreeUri(Uri uri) {
        treeUri = uri;
        int flags = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        resolver.takePersistableUriPermission(uri, flags);
        getPrefs().edit().putString(KEY_TREE_URI, uri.toString()).apply();
        Log.i(TAG, "Saved tree URI: " + uri);
    }

    /**
     * Scan SAF directory, open fds, store in pendingFds.
     * Does NOT call any JNI methods.
     */
    public ScanResult scanAndStore() {
        ScanResult result = new ScanResult();
        pendingFds.clear();

        if (treeUri == null) {
            result.error = "No tree URI set";
            return result;
        }

        String rootDocId = DocumentsContract.getTreeDocumentId(treeUri);
        Log.i(TAG, "Starting recursive scan, rootDocId=" + rootDocId);
        scanDir(rootDocId, "", result);

        for (String r : REQUIRED_FILES) {
            if (!result.found.contains(r)) result.missingRequired.add(r);
        }
        for (String o : OPTIONAL_FILES) {
            if (!result.found.contains(o)) result.missingOptional.add(o);
        }

        int musicCount = 0;
        for (String f : result.found) {
            if (f.startsWith("Music/rayman") && f.endsWith(".ogg")) musicCount++;
        }
        result.musicTracks = musicCount;
        result.hasMusicDir = musicCount > 0;

        Log.i(TAG, "Scan complete: " + result.found.size() + " found, "
                + result.missingRequired.size() + " missing required, "
                + pendingFds.size() + " fds pending");
        return result;
    }

    /* ── Phase 2: JNI registration (RayverseActivity, after SDL loads libs) ── */

    /**
     * Register all pending fds with native code.
     * Must be called AFTER System.loadLibrary("main") (i.e. from SDLActivity).
     */
    public void registerWithNative() {
        nativeSetSafUri(treeUri.toString());

        for (FdEntry entry : pendingFds) {
            nativeRegisterFd(entry.path, entry.fd, entry.mode);
        }
        Log.i(TAG, "Registered " + pendingFds.size() + " fds with native");

        String gamePath = treeUri.getPath();
        if (gamePath != null) {
            nativeSetGameDataPath(gamePath, 1);
        }

        File saveDir = new File(context.getFilesDir(), "rayverse_save");
        saveDir.mkdirs();
        nativeSetSaveDir(saveDir.getAbsolutePath());
    }

    /* ── Internal scanning ── */

    private void scanDir(String docId, String relPrefix, ScanResult result) {
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, docId);

        try (Cursor cursor = resolver.query(childrenUri,
                new String[]{
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE
                }, null, null, null)) {

            if (cursor == null) return;

            while (cursor.moveToNext()) {
                String childDocId = cursor.getString(0);
                String displayName = cursor.getString(1);
                String mimeType = cursor.getString(2);
                String relPath = relPrefix.isEmpty() ? displayName : relPrefix + "/" + displayName;

                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mimeType)) {
                    scanDir(childDocId, relPath, result);
                } else if (ALL_KNOWN_PATHS.contains(relPath) || isMusicOgg(relPath)) {
                    try {
                        Uri fileUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childDocId);
                        ParcelFileDescriptor pfd = resolver.openFileDescriptor(fileUri, "r");
                        if (pfd != null) {
                            int fd = pfd.detachFd();
                            pendingFds.add(new FdEntry(relPath, fd, 0));
                            result.found.add(relPath);
                            Log.i(TAG, "Opened fd " + fd + " for " + relPath);
                        }
                    } catch (Exception e) {
                        Log.e(TAG, "Failed to open " + relPath, e);
                    }
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "scanDir failed for " + relPrefix, e);
        }
    }

    private boolean isMusicOgg(String path) {
        return path.startsWith("Music/rayman") && path.endsWith(".ogg");
    }

    private SharedPreferences getPrefs() {
        return context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    /* ── Data classes ── */

    private static class FdEntry {
        final String path;
        final int fd;
        final int mode;
        FdEntry(String path, int fd, int mode) {
            this.path = path; this.fd = fd; this.mode = mode;
        }
    }

    public boolean hasSaveFiles() {
        File saveDir = new File(context.getFilesDir(), "rayverse_save");
        if (!saveDir.exists()) return false;
        String[] files = saveDir.list();
        if (files == null) return false;
        for (String f : files) {
            if (f.endsWith(".SAV") || f.endsWith(".CFG")) return true;
        }
        return false;
    }

    public static class ScanResult {
        public List<String> found = new ArrayList<>();
        public List<String> missingRequired = new ArrayList<>();
        public List<String> missingOptional = new ArrayList<>();
        public boolean hasMusicDir = false;
        public int musicTracks = 0;
        public String error = null;

        public boolean isComplete() {
            return error == null && missingRequired.isEmpty();
        }

        public String getSummary() {
            if (error != null) return "Error: " + error;
            StringBuilder sb = new StringBuilder();
            sb.append("Found: ").append(found.size()).append(" files\n");
            if (!missingRequired.isEmpty()) {
                sb.append("MISSING REQUIRED: ").append(missingRequired).append("\n");
            }
            if (!missingOptional.isEmpty()) {
                sb.append("Missing optional: ").append(missingOptional).append("\n");
            }
            if (hasMusicDir) {
                sb.append("CD tracks found: ").append(musicTracks).append("/19\n");
            }
            return sb.toString();
        }
    }
}
