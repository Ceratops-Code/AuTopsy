"""Public update-command and dialog cases with an owned release-feed fixture.

The normal desktop harness confines every window and deletes its private
profile. No browser, installer, network request or user project is involved.
"""
import json
import os
import sys
from pathlib import Path

from test_clip_actions import with_native_dialog
from test_video_transport import wait_for_native_state


def check_product_updates(client, root):
    if os.environ.get("AUTOPSY_TEST_UPDATES_SOURCE", "fixture") == "github":
        for _ in range(2):
            result = client.data("GetAuTopsyUpdates")
            assert result["current"] and not result["error"], result
        return
    fixture = root / "_updates.json"

    ubuntu = sys.platform.startswith("linux")

    def offer(tag, *, draft=False, supported_architecture=True, foreign=False, other_platform=False):
        page = "https://github.com/Ceratops-Code/AuTopsy/releases"
        if ubuntu != other_platform:
            name = "autopsy-editor_3.7.10~b1_" + ("amd64" if supported_architecture else "arm64") + ".deb"
        else:
            name = "autopsy-win-3.7.10-test-" + ("x64" if supported_architecture else "arm64") + ".exe"
        return {"tag_name": tag, "draft": draft, "prerelease": "-autopsy." in tag,
            "html_url": page + "/tag/" + tag, "body": "Neutral release notes for a newer AuTopsy preview.",
            "assets": [{"name": name, "state": "uploaded", "size": 1234,
                "browser_download_url": "https://example.org/installer.exe" if foreign
                    else page + "/download/" + tag + "/" + name}]}

    fixture.write_text("[]", encoding="utf-8")
    current = client.data("GetAuTopsyUpdates")["current"]
    metadata = Path(__file__).resolve().parents[2] / "resources/autopsy-release.json"
    assert current == json.loads(metadata.read_text())["version"]
    major, minor, patch = (int(piece) for piece in current.split("-autopsy.")[0].split("."))
    core = f"{major}.{minor}.{patch+1}"
    tag = f"v{core}-autopsy.1"
    future = f"v{core}-autopsy.2"
    good = [offer("v" + current), offer(tag), offer(future, draft=True),
            offer(future, supported_architecture=False), offer(future, foreign=True), offer(future, other_platform=True)]
    before = client.data("GetInfo: Type=Tracks Format=JSON")
    for _ in range(2):
        fixture.write_text(json.dumps(good), encoding="utf-8")
        result = client.data("GetAuTopsyUpdates")
        assert result["available"] and result["tag"] == tag and not result["error"], result
        assert result["download"].endswith("_amd64.deb" if ubuntu else "-x64.exe"), result
        assert not client.data("GetAuTopsyUpdates: Previews=0")["available"]
        fixture.write_text("not-json", encoding="utf-8")
        result = client.data("GetAuTopsyUpdates")
        assert result["error"] and not result["available"], result
        fixture.write_text(json.dumps([offer("v" + current)]), encoding="utf-8")
        result = client.data("GetAuTopsyUpdates")
        assert not result["error"] and not result["available"], result
        assert client.data("GetInfo: Type=Tracks Format=JSON") == before
    if ubuntu:
        return
    fixture.write_text(json.dumps(good), encoding="utf-8")

    def inspect(windows, dialog):
        wait_for_native_state(lambda: any("New release: " + tag in windows.caption(h)
            for h in windows.windows(dialog)), "newer AuTopsy release shown in its update dialog")
        automatic = windows.button(dialog, "Check automatically (public release metadata only)")
        previews = windows.button(dialog, "Include preview versions")
        assert windows.message(automatic, 0xF0) == 0, "Automatic checks must start disabled."
        for _ in range(2):
            windows.click(previews)
            wait_for_native_state(lambda: any("No newer Windows x64" in windows.caption(h)
                for h in windows.windows(dialog)), "stable channel excludes preview installers")
            windows.click(previews)
            wait_for_native_state(lambda: any("New release: " + tag in windows.caption(h)
                for h in windows.windows(dialog)), "preview channel restored")
        windows.click(windows.button(dialog, "Close"))

    with_native_dialog(lambda: client.send("CheckAuTopsyUpdates"), "AuTopsy Updates", inspect)
    assert client.data("GetInfo: Type=Tracks Format=JSON") == before
