// SPDX-License-Identifier: GPL-2.0-or-later
package com.astrobotquest.vrhost;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.Comparator;

/** Filesystem-only game selection, also testable without an Android SDK. */
final class GameSelection {
    private GameSelection() {}

    static File resolve(String requested, File external, File shellGames) throws IOException {
        if (requested != null && !requested.isEmpty()) {
            File selected = new File(requested);
            if (!selected.isAbsolute()) {
                selected = new File(external, requested);
            }
            selected = selected.getCanonicalFile();
            if (selected.isDirectory()) {
                selected = new File(selected, "eboot.bin");
            }
            if (!selected.getName().equals("eboot.bin") || !selected.isFile() ||
                    !selected.canRead()) {
                throw new IOException("game_path must name a readable eboot.bin or its folder: "
                        + selected);
            }
            // An explicit selection never silently falls back to another game.
            return selected;
        }
        File result = find(new File(external, "games"));
        return result != null ? result : find(shellGames);
    }

    private static File find(File games) {
        // Preserve the existing Astro Bot default when no game_path is set.
        File preferred = new File(games, "CUSA12392/eboot.bin");
        if (preferred.isFile() && preferred.canRead()) {
            return preferred;
        }
        File[] folders = games.listFiles();
        if (folders != null) {
            Arrays.sort(folders, Comparator.comparing(File::getName));
            for (File folder : folders) {
                File eboot = new File(folder, "eboot.bin");
                if (eboot.isFile() && eboot.canRead()) {
                    return eboot;
                }
            }
        }
        return null;
    }
}
