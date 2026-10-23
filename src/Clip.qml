import QtQuick
import Omotion

// A video's frame at its own time, chosen by number, so a scene stays a
// function of t: "time" is seconds into the clip, usually t minus when the
// clip starts. Before it, the first frame shows; after it, the last.
//
//   Clip { video: "assets/footage.mp4"; time: scene.t - 2; fps: scene.fps; width: 960; height: 540 }
Image {
    property string video
    property real time: 0
    property int fps: 30
    readonly property real length: video !== "" ? Clips.length(video, fps) : 0

    source: video !== "" ? Clips.frame(video, time, fps) : ""
    asynchronous: false
    cache: false
    smooth: true
    fillMode: Image.PreserveAspectCrop
}
