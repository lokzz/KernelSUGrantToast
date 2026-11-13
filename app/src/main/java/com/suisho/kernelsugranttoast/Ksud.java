package com.suisho.kernelsugranttoast;

import java.io.File;

/**
 * Locates the ksud binary for the running root implementation.
 *
 * <p>KernelSU installs it at {@code /data/adb/ksud}. BakaSU (formerly
 * ReSukiSU) keeps the same path on a normal install, but some builds only ship
 * the binary under {@code /data/adb/ksu/bin}. Resolving it once keeps the
 * module working on both instead of hardcoding a single path.</p>
 */
public final class Ksud {
    private static final String[] CANDIDATES = {
            "/data/adb/ksud",
            "/data/adb/ksu/bin/ksud"
    };
    private static volatile String resolvedPath;

    private Ksud() {
    }

    public static String getPath() {
        String path = resolvedPath;
        if (path != null) return path;
        synchronized (Ksud.class) {
            if (resolvedPath == null) {
                String found = CANDIDATES[0];
                for (String candidate : CANDIDATES) {
                    if (new File(candidate).exists()) {
                        found = candidate;
                        break;
                    }
                }
                resolvedPath = found;
            }
            return resolvedPath;
        }
    }
}
