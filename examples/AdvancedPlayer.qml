import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import MpvQml

Window {
    id: window
    visible: true
    width: 1280
    height: 720
    minimumWidth: 640
    minimumHeight: 360
    title: player.metadata.title || "MpvQml Advanced Player"
    color: "#11111b"

    property bool isAudioOnly: false
    property bool playlistFinished: false
    property bool hadVideo: false
    property int loopMode: 0  // 0=off, 1=file(inf), 2=file(N), 3=playlist(inf), 4=playlist(N)
    property int loopCount: 3

    MpvPlayer {
        id: player
        anchors.fill: parent
        fillMode: "preserveAspectCrop"

        Component.onCompleted: setProperty("keep-open", "always")
        onFileStarted: { playlistFinished = false; hadVideo = false }
        onFileLoaded: isAudioOnly = !player.hasVideo
        onHasVideoChanged: {
            isAudioOnly = !player.hasVideo
            if (hadVideo && !player.hasVideo)
                checkEndOfPlaylist()
            hadVideo = player.hasVideo
        }
        onPlaybackEnded: checkEndOfPlaylist()
        onEndFile: function(reason) {
            if (reason === "eof")
                checkEndOfPlaylist()
        }
        function checkEndOfPlaylist() {
            var items = player.playlist
            if (items.length <= 1 || items[items.length - 1].current)
                playlistFinished = true
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

    Rectangle {
        anchors.fill: parent
        visible: player.source === ""
        color: "#1e1e2e"
        z: 1

        Column {
            anchors.centerIn: parent
            spacing: 20

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "MpvQml Advanced Player"
                color: "white"
                font.pointSize: 24
                font.bold: true
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Drag a media file here, or use Open to browse"
                color: "#a6adc8"
                font.pointSize: 14
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Open File"
                onClicked: fileDialog.open()
            }
        }
    }

    Rectangle {
        id: audioFallback
        anchors.fill: parent
        color: "#1e1e2e"
        visible: player.source !== "" && !playlistFinished && isAudioOnly

        Column {
            anchors.centerIn: parent
            spacing: 15

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Audio Track Active"
                color: "white"
                font.pointSize: 20
                font.bold: true
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: {
                    var t = player.metadata.title
                    if (t) return t
                    var parts = player.source.toString().replace(/^file:\/\//, "").split("/")
                    return parts[parts.length - 1] || "Unknown File"
                }
                color: "#a6adc8"
                font.pointSize: 14
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: playlistFinished
        color: "#1e1e2e"
        z: 1

        Text {
            anchors.centerIn: parent
            text: "Playback Ended"
            color: "white"
            font.pointSize: 20
            font.bold: true
        }
    }

    Rectangle {
        id: controlBar
        height: 100
        color: "#B3181825"
        z: 2

        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        Column {
            anchors.centerIn: parent
            spacing: 6

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8

                Button {
                    text: "Open"
                    onClicked: fileDialog.open()
                }

                Button {
                    text: "|<"
                    enabled: player.source !== ""
                    onClicked: player.previous()
                }

                Button {
                    text: "-10s"
                    enabled: player.source !== ""
                    onClicked: player.seekRelative(-10.0)
                }

                Button {
                    text: player.paused ? "Play" : "Pause"
                    enabled: player.source !== ""
                    onClicked: player.paused = !player.paused
                }

                Button {
                    text: "+10s"
                    enabled: player.source !== ""
                    onClicked: player.seekRelative(10.0)
                }

                Button {
                    text: ">|"
                    enabled: player.source !== ""
                    onClicked: player.next()
                }

                Slider {
                    id: volumeSlider
                    from: 0; to: 150
                    value: player.volume
                    onMoved: player.volume = value
                    ToolTip {
                        parent: volumeSlider.handle
                        visible: volumeSlider.pressed
                        text: volumeSlider.value.toFixed(0)
                    }
                }

                Button {
                    text: "Snap"
                    enabled: player.source !== ""
                    onClicked: player.screenshot()
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8

                Button {
                    text: ["Loop: Off", "Loop: File", "Loop: File (" + loopCount + "x)", "Loop: Playlist", "Loop: Playlist (" + loopCount + "x)"][loopMode]
                    enabled: player.source !== ""
                    onClicked: {
                        loopMode = (loopMode + 1) % 5
                        player.loopFile = loopMode === 1 ? "inf" : loopMode === 2 ? loopCount : "no"
                        player.loopPlaylist = loopMode === 3 ? "inf" : loopMode === 4 ? loopCount : "no"
                    }
                }

                Button {
                    text: ["Fit", "Crop", "Stretch"][player.fillMode === "preserveAspectFit" ? 0 : player.fillMode === "preserveAspectCrop" ? 1 : 2]
                    enabled: player.source !== ""
                    onClicked: {
                        var modes = ["preserveAspectFit", "preserveAspectCrop", "stretch"]
                        var idx = modes.indexOf(player.fillMode)
                        player.fillMode = modes[(idx + 1) % modes.length]
                    }
                }

                Button {
                    id: markButton
                    text: "Mark"
                    enabled: player.source !== ""
                    onClicked: {
                        player.markSeekPosition()
                        revertButton.enabled = true
                    }
                }

                Button {
                    id: revertButton
                    text: "Undo Seek"
                    enabled: false
                    onClicked: {
                        player.revertSeek()
                        enabled = false
                    }
                }

                Button {
                    id: audioMenuButton
                    text: "Audio"
                    enabled: player.source !== ""
                    onClicked: audioMenu.open()

                    Menu {
                        id: audioMenu
                        y: -audioMenu.height

                        Repeater {
                            model: player.trackList
                            MenuItem {
                                visible: modelData.type === "audio"
                                text: (modelData.selected ? "* " : "") + (modelData.codec || "audio") + (modelData.lang ? " (" + modelData.lang + ")" : "")
                                onClicked: player.selectTrack("audio", modelData.trackId)
                            }
                        }
                    }
                }

                Text { text: "a-delay"; color: "white"; anchors.verticalCenter: parent.verticalCenter }
                SpinBox {
                    from: -50; to: 50
                    value: player.audioDelay * 10
                    onValueModified: player.audioDelay = value / 10
                }

                Button {
                    text: window.visibility === Window.FullScreen ? "Exit FS" : "Fullscreen"
                    onClicked: window.visibility = window.visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen
                }
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
