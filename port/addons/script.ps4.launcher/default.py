import json
import os

import xbmc
import xbmcaddon
import xbmcgui
import xbmcvfs

ADDON = xbmcaddon.Addon()
APPS_JSON = "special://userdata/ps4-apps.json"


def load_json_apps():
    path = xbmcvfs.translatePath(APPS_JSON)
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return []

    apps = []
    for entry in data if isinstance(data, list) else []:
        if not isinstance(entry, dict):
            continue
        title_id = str(entry.get("title_id", "")).strip().upper()
        if len(title_id) != 9 or not title_id.isalnum():
            continue
        name = str(entry.get("name", title_id)).strip() or title_id
        apps.append((name, title_id))
    return apps


def scan_installed_apps():
    apps = []
    try:
        names = os.listdir("/user/app")
    except OSError:
        return apps

    for name in sorted(names):
        if len(name) == 9 and name.isalnum():
            apps.append((name, name.upper()))
    return apps


def merge_apps(primary, fallback):
    seen = {title_id for _, title_id in primary}
    merged = list(primary)
    for name, title_id in fallback:
        if title_id not in seen:
            merged.append((name, title_id))
            seen.add(title_id)
    return merged


def main():
    apps = merge_apps(load_json_apps(), scan_installed_apps())
    if not apps:
        xbmcgui.Dialog().notification(
            "PS4 App Launcher",
            "No apps. Create special://userdata/ps4-apps.json",
            xbmcgui.NOTIFICATION_WARNING,
            5000,
        )
        return

    labels = ["{} ({})".format(name, title_id) for name, title_id in apps]
    index = xbmcgui.Dialog().select(ADDON.getAddonInfo("name"), labels)
    if index < 0:
        return

    title_id = apps[index][1]
    xbmc.log("PS4 launcher: launching {}".format(title_id), xbmc.LOGINFO)
    xbmc.executebuiltin("LaunchPS4App({})".format(title_id))


if __name__ == "__main__":
    main()
