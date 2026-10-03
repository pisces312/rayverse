package com.rayverse.rayman;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;

import androidx.documentfile.provider.DocumentFile;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

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
        RayLog.i(TAG, "Saved tree URI: " + uri);
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
        RayLog.i(TAG, "Starting recursive scan, rootDocId=" + rootDocId);
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

        RayLog.i(TAG, "Scan complete: " + result.found.size() + " found, "
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
        RayLog.i(TAG, "Registered " + pendingFds.size() + " fds with native");

        String gamePath = treeUri.getPath();
        if (gamePath != null) {
            nativeSetGameDataPath(gamePath, 1);
        }

        File saveDir = getSaveDir();
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
                } else {
                    try {
                        Uri fileUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childDocId);
                        ParcelFileDescriptor pfd = resolver.openFileDescriptor(fileUri, "r");
                        if (pfd != null) {
                            int fd = pfd.detachFd();
                            pendingFds.add(new FdEntry(relPath, fd, 0));
                            result.found.add(relPath);
                            RayLog.i(TAG, "Opened fd " + fd + " for " + relPath);
                        }
                    } catch (Exception e) {
                        RayLog.e(TAG, "Failed to open " + relPath, e);
                    }
                }
            }
        } catch (Exception e) {
            RayLog.e(TAG, "scanDir failed for " + relPrefix, e);
        }
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

    public File getSaveDir() {
        File saveDir = new File(context.getFilesDir(), "rayverse_save");
        saveDir.mkdirs();
        return saveDir;
    }

    public static boolean isSaveFileName(String name) {
        if (name == null) return false;
        String u = name.toUpperCase(Locale.ROOT);
        return u.startsWith("RAYMAN") && (u.endsWith(".SAV") || u.endsWith(".CFG"));
    }

    public List<String> listSaveFiles() {
        List<String> out = new ArrayList<>();
        File[] files = getSaveDir().listFiles();
        if (files == null) return out;
        for (File f : files) {
            if (f.isFile() && isSaveFileName(f.getName())) out.add(f.getName());
        }
        Collections.sort(out);
        return out;
    }

    public boolean hasSaveFiles() {
        return !listSaveFiles().isEmpty();
    }

    /** Copy all save files into the SAF folder at treeUri. Returns count exported. */
    public int exportSavesTo(Uri treeUri) {
        DocumentFile dest = DocumentFile.fromTreeUri(context, treeUri);
        if (dest == null) return -1;
        int n = 0;
        for (String name : listSaveFiles()) {
            DocumentFile target = dest.findFile(name);
            if (target == null) target = dest.createFile("application/octet-stream", name);
            if (target == null) continue;
            try (InputStream in = new FileInputStream(new File(getSaveDir(), name));
                 OutputStream out = resolver.openOutputStream(target.getUri(), "wt")) {
                copy(in, out);
                n++;
            } catch (IOException e) {
                RayLog.e(TAG, "export failed: " + name, e);
            }
        }
        RayLog.i(TAG, "Exported " + n + " save file(s)");
        return n;
    }

    /** Copy RAYMAN*.SAV/.CFG from the SAF folder at treeUri into the save dir (overwrite). Returns count imported. */
    public int importSavesFrom(Uri treeUri) {
        DocumentFile srcDir = DocumentFile.fromTreeUri(context, treeUri);
        if (srcDir == null) return -1;
        int n = 0;
        for (DocumentFile f : srcDir.listFiles()) {
            if (!f.isFile()) continue;
            String name = f.getName();
            if (!isSaveFileName(name)) continue;
            try (InputStream in = resolver.openInputStream(f.getUri());
                 OutputStream out = new FileOutputStream(new File(getSaveDir(), name))) {
                copy(in, out);
                n++;
            } catch (IOException e) {
                RayLog.e(TAG, "import failed: " + name, e);
            }
        }
        RayLog.i(TAG, "Imported " + n + " save file(s)");
        return n;
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[8192];
        int r;
        while ((r = in.read(buf)) != -1) out.write(buf, 0, r);
        out.flush();
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
