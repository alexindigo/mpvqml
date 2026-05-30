import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import MpvQml

Window {
    id: window
    visible: true
    width: 1920
    height: 1080
    visibility: Window.FullScreen
    color: "#c0c0c0"

    property bool hasMedia: false
    property var mediaFilters: ["*.jpg", "*.jpeg", "*.png", "*.bmp",
        "*.gif", "*.webp", "*.mp4", "*.mkv", "*.avi", "*.mov", "*.webm"]

    function loadDir(path) {
        hasMedia = true
        player.setProperty("image-display-duration", "6")
        player.loopPlaylist = "inf"
        player.loadDirectory(path, mediaFilters, "replace", "name")
    }

    MpvPlayer {
        id: player
        anchors.fill: parent
        fillMode: "preserveAspectCrop"
        visible: hasMedia
        mute: true
    }

    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: function(drag) {
            if (drag.urls.length > 0)
                loadDir(drag.urls[0])
        }
    }

    FolderDialog {
        id: folderDialog
        title: "Select Media Directory"
        onAccepted: loadDir(folderDialog.selectedFolder)
    }

    property var fileMetaLines: {
        var lines = []
        var m = player.metadata
        if (m) {
            var keys = Object.keys(m)
            for (var i = 0; i < keys.length; i++)
                lines.push(keys[i] + ": " + m[keys[i]])
        }
        return lines
    }
    property var trackLines: {
        var tlines = []
        var tracks = player.trackList
        for (var t = 0; t < tracks.length; t++) {
            var track = tracks[t]
            var lines = []
            var keys = Object.keys(track)
            for (var k = 0; k < keys.length; k++) {
                var key = keys[k]
                if (key === "type" || key === "trackId") continue
                if (typeof track[key] === "object") continue
                lines.push(key + ": " + track[key])
            }
            if (track.metadata) {
                var mk = Object.keys(track.metadata)
                for (var k = 0; k < mk.length; k++)
                    lines.push(mk[k] + ": " + track.metadata[mk[k]])
            }
            tlines.push(lines)
        }
        return tlines
    }

    Rectangle {
        id: metaBox
        z: 1
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        visible: hasMedia
        width: metaCol.width + 16
        height: Math.min(metaCol.height + 12, parent.height * 0.75)
        radius: 4
        clip: true
        color: "#a0c0c0c0"
        border.color: "#808080"
        border.width: 1

        MouseArea { anchors.fill: parent; onClicked: {} }

        Column {
            id: metaCol
            anchors.top: parent.top
            anchors.topMargin: 0
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 12

            Column {
                spacing: 1
                Text {
                    id: fileHeader
                    text: String(player.source).split("/").pop() + "  [-]"
                    color: "#000080"; font.pixelSize: 13; font.bold: true
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            fileMetaContent.visible = !fileMetaContent.visible
                            fileHeader.text = String(player.source).split("/").pop() + (fileMetaContent.visible ? "  [-]" : "  [+]")
                        }
                    }
                }

                Column {
                    id: fileMetaContent
                    visible: true
                    spacing: 1
                    Repeater {
                        model: fileMetaLines
                        delegate: Text { text: modelData; color: "#000000"; font.pixelSize: 12 }
                    }
                }
            }

            Repeater {
                model: trackLines
                delegate: Column {
                    spacing: 1
                    readonly property var thisTrack: modelData

                    Rectangle { height: 1; width: 220; color: "#808080"; visible: index > 0 }

                    Text {
                        id: trackHeader
                        text: (index + 1) + ". " + player.trackList[index].type + "  [-]"
                        color: "#000080"; font.pixelSize: 12; font.bold: true
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                trackContent.visible = !trackContent.visible
                                trackHeader.text = (index + 1) + ". " + player.trackList[index].type + (trackContent.visible ? "  [-]" : "  [+]")
                            }
                        }
                    }

                    Column {
                        id: trackContent
                        visible: false
                        spacing: 1
                        Repeater {
                            model: thisTrack
                            delegate: Text { text: modelData; color: "#000000"; font.pixelSize: 12 }
                        }
                    }
                }
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: folderDialog.open()

        Column {
            id: overlay
            anchors.centerIn: parent
            visible: !hasMedia
            spacing: 8

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "drop a folder or click to open one"
                color: "#000000"
                font.pixelSize: 20
                font.bold: true
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "space to pause  |  arrows to navigate  |  ESC to exit"
                color: "#0000ff"
                font.pixelSize: 13
            }
        }
    }

    Item {
        focus: true
        Keys.onEscapePressed: Qt.quit()
        Keys.onLeftPressed: if (hasMedia) player.previous()
        Keys.onRightPressed: if (hasMedia) player.next()
        Keys.onSpacePressed: if (hasMedia) player.paused = !player.paused
    }
}
