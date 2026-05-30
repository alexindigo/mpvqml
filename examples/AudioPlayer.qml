import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import MpvQml

Window {
    id: window
    visible: true
    width: 520
    height: 360
    title: "Audio Player"
    color: "#c0c0c0"

    MpvPlayer {
        id: player
        anchors.fill: parent
        visible: source !== ""
        fillMode: "preserveAspectFit"
        Component.onCompleted: {
            setProperty("keep-open", "always")
        }
    }

    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: function(drag) {
            if (drag.urls.length > 0)
                player.source = drag.urls[0]
        }
    }

    Column {
        anchors.centerIn: parent
        visible: player.source === ""
        spacing: 12

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Audio Player"
            font.pixelSize: 20
            font.bold: true
            color: "#000000"
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "drop a file or click to open"
            color: "#000080"
            font.pixelSize: 13
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "open"
            onClicked: fileDialog.open()
        }
    }

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 10
        anchors.topMargin: 10
        width: metaText.width + 12
        height: metaText.height + 8
        radius: 4
        color: "#a0c0c0c0"
        border.color: "#808080"
        border.width: 1

        Text {
            id: metaText
            anchors.centerIn: parent
            color: "#000000"
            font.pixelSize: 12
            text: {
                var name = String(player.source).split("/").pop()
                if (!player.metadata)
                    return name
                var lines = [name]
                for (var key in player.metadata)
                    lines.push(key + ": " + player.metadata[key])
                return lines.join("\n")
            }
        }
    }

    Rectangle {
        id: controls
        height: 120
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        color: "#a0c0c0c0"

        Column {
            anchors.centerIn: parent
            spacing: 6

            Row {
                spacing: 6

                Button {
                    text: player.paused ? "play" : "pause"
                    enabled: player.source !== ""
                    onClicked: player.paused = !player.paused
                }
                Button {
                    text: "-5s"
                    enabled: player.source !== ""
                    onClicked: player.seekRelative(-5.0)
                }
                Button {
                    text: "+5s"
                    enabled: player.source !== ""
                    onClicked: player.seekRelative(5.0)
                }
                Button {
                    text: "0"
                    enabled: player.source !== ""
                    onClicked: player.seekAbsolute(0)
                }
            }

            Row {
                spacing: 6

                Text { text: "vol"; color: "#000000"; anchors.verticalCenter: parent.verticalCenter }
                Slider {
                    id: volSlider
                    width: 100
                    from: 0; to: 150; value: player.volume
                    onMoved: player.volume = value
                }

                Text { text: "speed"; color: "#000000"; anchors.verticalCenter: parent.verticalCenter }
                Slider {
                    id: speedSlider
                    width: 100
                    from: 0.25; to: 3.0; stepSize: 0.25; value: player.playbackSpeed
                    onMoved: player.playbackSpeed = value
                }
                Text {
                    text: speedSlider.value.toFixed(2) + "x"
                    color: "#000000"
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Row {
                spacing: 6

                Text {
                    text: {
                        var p = player.position
                        var d = player.duration
                        if (d <= 0) return "--:--"
                        return Math.floor(p / 60) + ":" + String(Math.floor(p % 60)).padStart(2, "0")
                            + "." + String(Math.floor(p * 100 % 100)).padStart(2, "0")
                            + " / " + Math.floor(d / 60) + ":" + String(Math.floor(d % 60)).padStart(2, "0")
                            + "." + String(Math.floor(d * 100 % 100)).padStart(2, "0")
                    }
                    color: "#000000"
                    anchors.verticalCenter: parent.verticalCenter
                }
                Slider {
                    id: seekSlider
                    width: 200
                    from: 0; to: player.duration > 0 ? player.duration : 1
                    value: player.position
                    onMoved: if (pressed) player.seekAbsolute(value)
                }
            }

            Row {
                spacing: 6

                Row {
                    spacing: 4
                    CheckBox {
                        checked: player.audioPitchCorrection
                        onClicked: player.audioPitchCorrection = checked
                    }
                    Text { text: "pitch correction"; color: "#000000"; anchors.verticalCenter: parent.verticalCenter }
                }


            }
        }
    }

    FileDialog {
        id: fileDialog
        title: "Open Audio File"
        nameFilters: ["Audio Files (*.mp3 *.MP3 *.m4a *.M4A *.flac *.FLAC *.ogg *.OGG *.wav *.WAV *.aac *.AAC *.wma *.WMA)", "All Files (*)"]
        onAccepted: player.source = selectedFile
    }
}
