package com.astrobotquest.vrhost;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;

public final class GameSelectionTest {
    private static File game(Path root, String name) throws IOException {
        Path folder = Files.createDirectories(root.resolve(name));
        return Files.createFile(folder.resolve("eboot.bin")).toFile();
    }

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("quest-game-selection");
        try {
            File external = Files.createDirectories(root.resolve("external")).toFile();
            File shell = Files.createDirectories(root.resolve("shell")).toFile();
            assert GameSelection.resolve(null, external, shell) == null;
            File shellGame = game(shell.toPath(), "CUSA22222");
            assert GameSelection.resolve(null, external, shell).equals(shellGame);
            File first = game(external.toPath().resolve("games"), "CUSA11111");
            game(external.toPath().resolve("games"), "CUSA99999");
            assert GameSelection.resolve(null, external, shell).equals(first);
            File astro = game(external.toPath().resolve("games"), "CUSA12392");
            assert GameSelection.resolve("", external, shell).equals(astro);
            assert GameSelection.resolve("games/CUSA11111", external, shell).equals(first);
            assert GameSelection.resolve("games/CUSA11111/eboot.bin", external, shell).equals(first);
            assert GameSelection.resolve(shellGame.getAbsolutePath(), external, shell).equals(shellGame);
            File spaced = game(root, "Game with spaces");
            assert GameSelection.resolve(spaced.getParent(), external, shell).equals(spaced);
            for (String invalid : new String[]{"missing", "games", "games/CUSA11111/param.sfo"}) {
                boolean failed = false;
                try {
                    GameSelection.resolve(invalid, external, shell);
                } catch (IOException expected) {
                    failed = true;
                }
                assert failed : "Explicit invalid selection must not fall back: " + invalid;
            }
            System.out.println("Quest game selection: passed (explicit paths, defaults, invalid selection)");
        } finally {
            try (var paths = Files.walk(root)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) {
                    Files.delete(path);
                }
            }
        }
    }
}
