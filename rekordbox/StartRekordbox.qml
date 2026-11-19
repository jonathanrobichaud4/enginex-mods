import QtQuick 2.12
import QtQuick.Layouts 1.12
import Planck 1.0
import "qrc:/Skins/Embedded/Views/ControlCenter/Tabs/Preferences/Entries/Base"

// rekordbox mod: the entry under Hardware Diagnostics in Settings > ABOUT / UPDATE. The About
// tab (shared with sc-diagnostics) loads it from the launcher, http://127.0.0.1:8093/entry.qml,
// because Engine only finds QML files it already has. It asks the mod's launcher (port 8093) what state things are in. Set up looks for
// rekordbox-pack.tgz on a USB drive; Start stops Engine and runs rekordbox, and Engine comes
// back when rekordbox quits (SHIFT + BACK for two seconds).
PrefsButtonEntry {
    id: entry
    property string rbState: ""
    property string rbMessage: ""
    property bool rbInstalled: false
    property bool waitingForSetup: false

    text: {
        switch (rbState) {
        case "missing": return "rekordbox (not set up)"
        case "installing": return "rekordbox (installing...)"
        case "working": return "rekordbox (looking for the pack...)"
        case "starting": return "rekordbox (starting...)"
        case "error": return rbInstalled ? "rekordbox (didn't start last time)" : "rekordbox (not set up)"
        case "": return "rekordbox (mod not running)"
        default: return "rekordbox"
        }
    }
    buttonText: rbInstalled ? "Start" : "Set up"
    enabled: rbState === "ready" || rbState === "missing" || rbState === "error"

    function request(method, path, done) {
        var x = new XMLHttpRequest()
        x.onreadystatechange = function() {
            if (x.readyState !== XMLHttpRequest.DONE) return
            var r = null
            try { r = JSON.parse(x.responseText) } catch (e) {}
            if (done) done(r)
        }
        x.open(method, "http://127.0.0.1:8093" + path)
        x.send("")
    }

    function refresh() {
        request("GET", "/status", function(r) {
            var before = rbState
            rbState = r && r.state ? r.state : ""
            rbMessage = r && r.message ? r.message : ""
            rbInstalled = !!(r && r.installed)
            if (waitingForSetup && (rbState === "ready" || rbState === "error")) {
                waitingForSetup = false
                PlanckGUI.views.prompts.post({
                    id: "RekordboxSetup",
                    title: rbState === "ready" ? "rekordbox is set up" : "rekordbox",
                    message: rbState === "ready" ? "Press Start to run it." : rbMessage,
                    options: Prompt.Ok
                })
            }
        })
    }

    onClicked: {
        if (!rbInstalled) waitingForSetup = true
        request("POST", "/start", function(r) { refresh() })
    }

    Timer {
        interval: 1500
        repeat: true
        running: entry.visible
        triggeredOnStart: true
        onTriggered: entry.refresh()
    }
}
