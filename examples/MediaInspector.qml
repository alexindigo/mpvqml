import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import MpvQml

Window {
    id: window
    visible: true
    width: 900
    height: 640
    title: "Media Inspector"
    color: "#c0c0c0"

    MpvPlayer {
        id: player
        anchors.fill: parent
        fillMode: fillToggle.checked ? "preserveAspectCrop" : "preserveAspectFit"

        function refreshTracks() {
            trackListModel.clear()
            var list = player.trackList
            for (var i = 0; i < list.length; ++i) {
                var t = list[i]
                trackListModel.append({ num: i + 1, type: t.type, lang: t.lang || "", trackId: t.trackId, selected: t.selected, codec: t.codec, codecDesc: t.codecDesc || "" })
            }
        }

        onFileLoaded: refreshTracks()
        onTrackListChanged: refreshTracks()
        onPlaybackEnded: {
            if (!loopToggle.checked)
                paused = true
        }
    }

    ListModel { id: trackListModel }

    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: function(drag) {
            if (drag.urls.length > 0)
                player.source = drag.urls[0]
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: player.source === ""
        color: "#c0c0c0"
        Column {
            anchors.centerIn: parent; spacing: 12
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Media Inspector"; font.pixelSize: 20; font.bold: true; color: "#000000" }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "drop a file or click to open"; color: "#000080"; font.pixelSize: 13 }
            Button { anchors.horizontalCenter: parent.horizontalCenter; text: "open"; onClicked: fileDialog.open() }
        }
    }

    Rectangle {
        id: sidebar
        width: 260
        anchors.top: parent.top
        anchors.bottom: bottomBar.top
        anchors.left: parent.left
        color: "#a0c0c0c0"
        clip: true

        Column {
            width: parent.width
            spacing: 4
            padding: 6

            Text { text: "Tracks"; font.bold: true; color: "#000000"; font.pixelSize: 12; style: Text.Raised; styleColor: "#e0e0e0" }
            Rectangle { height: 1; width: parent.width - 12; color: "#808080" }

            ListView {
                id: trackView
                width: parent.width - 12
                height: 200
                clip: true
                model: trackListModel
                delegate: Row {
                    spacing: 4; width: trackView.width
                    Text {
                        text: trackId + ". " + type + " " + codec
                              + (lang ? " (" + lang + ")" : "")
                        color: "#000000"
                        font.pixelSize: 11; style: Text.Raised; styleColor: "#e0e0e0"
                        elide: Text.ElideRight
                        width: trackView.width - 52
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    CheckBox {
                        checked: selected
                        height: 20; width: 20
                        anchors.verticalCenter: parent.verticalCenter
                        ToolTip.visible: hovered
                        ToolTip.text: checked ? "Disable track #" + trackId : "Enable track #" + trackId
                        onClicked: {
                            if (checked)
                                player.selectTrack(type, trackId)
                            else
                                player.selectTrack(type, 0)
                        }
                    }
                }
                ScrollIndicator.vertical: ScrollIndicator {}
            }

            Text { text: "Video Processing"; font.bold: true; color: "#000000"; font.pixelSize: 12; style: Text.Raised; styleColor: "#e0e0e0" }
            Rectangle { height: 1; width: parent.width - 12; color: "#808080" }

            Row { spacing: 4
                Text { text: "hwdec"; color: "#000000"; font.pixelSize: 11; style: Text.Raised; styleColor: "#e0e0e0"; anchors.verticalCenter: parent.verticalCenter }
                ComboBox {
                    id: hwdecBox; font.pixelSize: 11
                    model: ["auto", "yes", "no", "auto-copy", "vaapi", "vdpau", "nvdec", "cuda", "videotoolbox"]
                    onActivated: player.hwdec = currentText
                }
            }

            Row { spacing: 4
                CheckBox { id: interpBox; text: "interpolation"; font.pixelSize: 11; onClicked: player.interpolation = checked }
            }

            Row { spacing: 4
                Text { text: "tscale"; color: "#000000"; font.pixelSize: 11; style: Text.Raised; styleColor: "#e0e0e0"; anchors.verticalCenter: parent.verticalCenter }
                ComboBox {
                    id: tscaleBox; font.pixelSize: 11
                    model: ["mitchell", "catmull_rom", "oversample", "linear", "spline16", "spline36", "spline64", "lanczos", "gaussian", "bicubic", "ewa_lanczossharp"]
                    onActivated: player.tscale = currentText
                }
            }

            Text { text: "Adjustments"; font.bold: true; color: "#000000"; font.pixelSize: 12; style: Text.Raised; styleColor: "#e0e0e0" }
            Rectangle { height: 1; width: parent.width - 12; color: "#808080" }

            Column { spacing: 2
                Row { spacing: 4; Text { text: "brightness"; color: "#000000"; font.pixelSize: 11; style: Text.Raised; styleColor: "#e0e0e0"; width: 70 }
                    Slider { from: -100; to: 100; value: player.brightness; onMoved: player.brightness = value; width: 140 }
                }
                Row { spacing: 4; Text { text: "contrast"; color: "#000000"; font.pixelSize: 11; style: Text.Raised; styleColor: "#e0e0e0"; width: 70 }
                    Slider { from: -100; to: 100; value: player.contrast; onMoved: player.contrast = value; width: 140 }
                }
                Row { spacing: 4; Text { text: "zoom"; color: "#000000"; font.pixelSize: 11; style: Text.Raised; styleColor: "#e0e0e0"; width: 70 }
                    Slider { from: -1; to: 1; stepSize: 0.01; value: player.videoZoom; onMoved: player.videoZoom = value; width: 140 }
                }
                Row { spacing: 4; Text { text: "pan X"; color: "#000000"; font.pixelSize: 11; style: Text.Raised; styleColor: "#e0e0e0"; width: 70 }
                    Slider { from: -1; to: 1; stepSize: 0.01; value: player.videoPanX; onMoved: player.videoPanX = value; width: 140 }
                }
                Row { spacing: 4; Text { text: "pan Y"; color: "#000000"; font.pixelSize: 11; style: Text.Raised; styleColor: "#e0e0e0"; width: 70 }
                    Slider { from: -1; to: 1; stepSize: 0.01; value: player.videoPanY; onMoved: player.videoPanY = value; width: 140 }
                }
            }
        }
    }

    Rectangle {
        id: bottomBar
        height: 60
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        color: "#a0c0c0c0"

        Column {
            anchors.centerIn: parent
            spacing: 2

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 6

                Button { text: "|<"; enabled: player.source !== ""; onClicked: player.previous() }
                Button { text: "-1f"; enabled: player.source !== ""; onClicked: player.frameStep() }
                Button {
                    text: player.paused ? "play" : "pause"
                    enabled: player.source !== ""
                    onClicked: player.paused = !player.paused
                }
                Button { text: "+1f"; enabled: player.source !== ""; onClicked: player.frameBackStep() }
                Button { text: ">|"; enabled: player.source !== ""; onClicked: player.next() }
                Button {
                    text: "snap"; enabled: player.source !== ""
                    onClicked: player.screenshot()
                }
                CheckBox { id: fillToggle; text: "crop"; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
                CheckBox { id: loopToggle; text: "loop"; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter; onClicked: player.loopFile = checked ? "inf" : "no" }
            }

            Text {
                id: detailText
                readonly property string videoCodec: {
                    for (var t of player.trackList)
                        if (t.type === "video" && t.selected)
                            return t.codecDesc || t.codec || "?"
                    return "?"
                }
                readonly property string audioCodec: {
                    for (var t of player.trackList)
                        if (t.type === "audio" && t.selected)
                            return t.codecDesc || t.codec || "?"
                    return "?"
                }
                text: player.source
                    ? "res: " + player.videoWidth + "x" + player.videoHeight
                        + " | " + videoCodec
                        + (audioCodec !== "?" ? " + " + audioCodec : "")
                        + " | hwdec: " + player.hwdecCurrent
                    : "drop a file"
                color: "#000000"
                font.pixelSize: 11
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: "Open Media File"
        nameFilters: ["Media Files (*.mp4 *.MP4 *.mkv *.MKV *.avi *.AVI *.mov *.MOV *.webm *.WEBM *.mp3 *.MP3 *.m4a *.M4A *.flac *.FLAC *.ogg *.OGG)", "All Files (*)"]
        onAccepted: player.source = selectedFile
    }
}
