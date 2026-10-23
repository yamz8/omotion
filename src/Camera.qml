import QtQuick
import Omotion

// A camera over what's inside it: pan and zoom, computed from t like the rest
// of the scene. Each key puts a point of the content at the frame's center,
// that close, by its time; the camera eases there over "over" seconds (1 by
// default), slowly enough to keep the viewer oriented.
//
//   Camera {
//       anchors.fill: parent
//       t: scene.t
//       keys: [{ at: 0, x: 960, y: 540, zoom: 1 }, { at: 4, x: 1300, y: 500, zoom: 1.4, over: 1.5 }]
//       Item {
//           readonly property var view: camera.plane(0.4)
//           x: view.x; y: view.y; scale: view.scale; transformOrigin: Item.TopLeft
//       }
//   }
//
// plane(depth) places a layer that the camera pans and zooms less (depth
// below 1, far away; 0 stays with the frame) or more (above 1, close up);
// 1 moves with the content.
Item {
    id: camera

    property real t: 0
    property var keys: []
    default property alias content: stage.data

    readonly property var view: {
        const rest = { x: width / 2, y: height / 2, zoom: 1 }
        if (!keys || keys.length === 0)
            return rest
        const filled = keys.map(k => ({ at: k.at, over: k.over === undefined ? 1 : k.over,
                                        x: k.x === undefined ? rest.x : k.x, y: k.y === undefined ? rest.y : k.y,
                                        zoom: k.zoom === undefined ? 1 : k.zoom }))
        return Motion.between(filled, t)
    }

    function plane(depth) {
        // The layer's own zoom and focus, part of the way to the camera's,
        // undone from the stage's transform the layer sits in.
        const zoom = 1 + (view.zoom - 1) * depth
        const focusX = width / 2 + (view.x - width / 2) * depth
        const focusY = height / 2 + (view.y - height / 2) * depth
        const scale = zoom / view.zoom
        return { x: view.x - scale * focusX, y: view.y - scale * focusY, scale: scale }
    }

    clip: true

    Item {
        id: stage
        width: camera.width
        height: camera.height
        transformOrigin: Item.TopLeft
        scale: camera.view.zoom
        x: camera.width / 2 - camera.view.x * camera.view.zoom
        y: camera.height / 2 - camera.view.y * camera.view.zoom
    }
}
