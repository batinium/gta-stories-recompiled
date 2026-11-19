package io.github.batinium.phonebuildprobe;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import android.util.Log;
import java.io.File;
import java.util.Map;

/** Runs files/test.sh in the app's own (untrusted_app) SELinux domain, output to files/out.txt. Env: NATIVE_DIR (own
 *  nativeLibraryDir), ADDON_DIR (the other probe package's nativeLibraryDir, if installed), FILES, CACHE. */
public class Run extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        final File files = getFilesDir();
        new Thread(() -> {
            try {
                ProcessBuilder pb = new ProcessBuilder("/system/bin/sh", new File(files, "test.sh").getPath());
                Map<String, String> env = pb.environment();
                env.put("NATIVE_DIR", getApplicationInfo().nativeLibraryDir);
                env.put("FILES", files.getPath());
                env.put("CACHE", getCacheDir().getPath());
                for (String other : new String[] {"io.github.batinium.phonebuildprobe", "io.github.batinium.phonebuildhost"}) {
                    if (other.equals(getPackageName())) continue;
                    try {
                        ApplicationInfo ai = getPackageManager().getApplicationInfo(other, 0);
                        env.put("ADDON_DIR", ai.nativeLibraryDir);
                    } catch (Exception e) {
                        env.put("ADDON_ERR", e.toString());
                    }
                }
                pb.redirectErrorStream(true);
                pb.redirectOutput(new File(files, "out.txt"));
                int rc = pb.start().waitFor();
                Log.i("phonebuild", "test.sh rc=" + rc);
            } catch (Exception e) {
                Log.e("phonebuild", "failed", e);
            }
        }).start();
    }
}
