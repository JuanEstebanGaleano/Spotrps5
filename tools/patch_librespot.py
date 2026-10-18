#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Spotrps5 contributors
"""Patches a librespot checkout (src/main.rs) so that the title can drive it: commands are read from stdin, one per line.

  python3 tools/patch_librespot.py path/to/librespot

Commands: token <file> <nonce> (Web API access token into a file), play_pause, play, pause, next, prev, vol_up, vol_down, shuffle_on, shuffle_off, repeat_on, repeat_off, repeat_track_on, repeat_track_off, volume <0-100>, seek <ms>.
Fails loudly when the code it patches is not where it expects it (a different librespot revision).
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "src/main.rs"
text = path.read_text()

LOOP = "    loop {\n        tokio::select! {\n            credentials = async {"
assert text.count(LOOP) == 1, "main loop not found"
READER = """    // Spotrps5: commands from the title, one per line on stdin.
    let (command_tx, mut command_rx) = tokio::sync::mpsc::unbounded_channel::<String>();
    std::thread::spawn(move || {
        use std::io::BufRead;
        let stdin = std::io::stdin();
        for line in stdin.lock().lines() {
            match line {
                Ok(line) => {
                    if command_tx.send(line).is_err() {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
    });

"""
text = text.replace(LOOP, READER + LOOP, 1)

ARM_ANCHOR = "            _ = tokio::signal::ctrl_c() => {\n                break;\n            },"
assert text.count(ARM_ANCHOR) == 1, "ctrl_c arm not found"
ARM = """            Some(command) = command_rx.recv() => {
                // "token <file> <nonce>": a Web API access token, written to <file> as "<nonce>\\n<seconds left>\\n<token>\\nEND\\n".
                if let Some(rest) = command.trim().strip_prefix("token ") {
                    let mut parts = rest.split_whitespace();
                    if let (Some(file), Some(nonce)) = (parts.next(), parts.next()) {
                        let scopes = "user-read-private,user-read-email,user-library-read,user-library-modify,playlist-read-private,playlist-read-collaborative,user-read-recently-played,user-top-read,user-read-playback-state,user-modify-playback-state,user-read-currently-playing,user-follow-read";
                        // Only once connected (the loop must not wait on a session that is not there), and never for long.
                        let answer = if spirc.is_some() && !session.is_invalid() {
                            tokio::time::timeout(std::time::Duration::from_secs(8), session.token_provider().get_token(scopes)).await.ok()
                        } else {
                            None
                        };
                        let text = match answer {
                            Some(Ok(token)) => format!("{nonce}\\n{}\\n{}\\nEND\\n", token.expires_in.as_secs(), token.access_token),
                            Some(Err(e)) => {
                                warn!("token request failed: {e}");
                                format!("{nonce}\\n0\\nERROR\\nEND\\n")
                            }
                            None => format!("{nonce}\\n0\\nERROR\\nEND\\n"),
                        };
                        if let Err(e) = std::fs::write(file, text) {
                            warn!("could not write {file}: {e}");
                        }
                    }
                    continue;
                }
                if let Some(spirc) = spirc.as_ref() {
                    let command = command.trim();
                    let result = match command {
                        "play_pause" => spirc.play_pause(),
                        "play" => spirc.play(),
                        "pause" => spirc.pause(),
                        "next" => spirc.next(),
                        "prev" => spirc.prev(),
                        "vol_up" => spirc.volume_up(),
                        "vol_down" => spirc.volume_down(),
                        "shuffle_on" => spirc.shuffle(true),
                        "shuffle_off" => spirc.shuffle(false),
                        "repeat_on" => spirc.repeat(true),
                        "repeat_off" => spirc.repeat(false),
                        "repeat_track_on" => spirc.repeat_track(true),
                        "repeat_track_off" => spirc.repeat_track(false),
                        c if c.starts_with("volume ") => {
                            let percent = c[7..].trim().parse::<u32>().unwrap_or(50).min(100);
                            spirc.set_volume((percent * 65535 / 100) as u16)
                        }
                        c if c.starts_with("seek ") => spirc.set_position_ms(c[5..].trim().parse::<u32>().unwrap_or(0)),
                        _ => Ok(()),
                    };
                    if let Err(e) = result {
                        warn!("command {command} failed: {e}");
                    }
                }
            },
"""
text = text.replace(ARM_ANCHOR, ARM + ARM_ANCHOR, 1)
path.write_text(text)
print("patched", path)
