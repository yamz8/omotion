import QtQuick
import QtQuick3D
import Omotion

// A scene's world in 3D: each world layer's mesh with its picture on it, put
// where the frame says, seen through the camera. Exports draw this same view
// without a window, so what's shown is what's written.
Item {
    id: view
    property var source: null
    readonly property var frame: source ? source.frame : ({})

    View3D {
        anchors.fill: parent
        camera: camera
        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Transparent
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        PerspectiveCamera {
            id: camera
            position: view.frame.cameraPosition || Qt.vector3d(0, 0, 0)
            rotation: view.frame.cameraRotation || Qt.quaternion(1, 0, 0, 0)
            fieldOfView: view.frame.fov || 40
            fieldOfViewOrientation: PerspectiveCamera.Vertical
            clipNear: 0.05
            clipFar: 5000
        }

        // Shadows on the ground, under the layers (drawn before them).
        Repeater3D {
            model: view.frame.shadows || []
            delegate: Model {
                required property var modelData
                source: "#Rectangle"
                position: modelData.position
                // The rectangle stands up facing the camera; laid flat on the
                // ground, then turned as the piece is. It's 100 units square.
                eulerRotation: Qt.vector3d(-90, -modelData.yaw, 0)
                scale: Qt.vector3d(modelData.width / 100, modelData.length / 100, 1)
                opacity: modelData.opacity
                depthBias: -20
                materials: DefaultMaterial {
                    lighting: DefaultMaterial.NoLighting
                    diffuseColor: "black"
                    diffuseMap: Texture { source: "qrc:/shadow.png" }
                }
            }
        }

        // Dust thrown up where things leave the ground and land: soft puffs
        // facing the camera, worked out for the frame's time (so the same
        // frame always shows the same dust).
        Model {
            visible: (view.frame.dustCount || 0) > 0
            geometry: DustGeometry { source: view.source }
            materials: CustomMaterial {
                shadingMode: CustomMaterial.Unshaded
                cullMode: Material.NoCulling
                depthDrawMode: Material.NeverDepthDraw
                sourceBlend: CustomMaterial.One
                destinationBlend: CustomMaterial.OneMinusSrcAlpha
                vertexShader: "qrc:/dust.vert"
                fragmentShader: "qrc:/dust.frag"
                property TextureInput sprite: TextureInput { texture: Texture { source: "qrc:/puff.png" } }
            }
        }

        Repeater3D {
            model: view.source ? view.source.layers : []
            delegate: Node {
                id: holder
                required property var modelData
                readonly property var place: (view.frame.places || {})[String(modelData.layer)] || ({})
                readonly property real size: place.scale === undefined ? 1 : place.scale
                position: place.position || Qt.vector3d(0, 0, 0)
                rotation: place.rotation || Qt.quaternion(1, 0, 0, 0)
                scale: Qt.vector3d(size, size, size)
                visible: place.visible !== false

                Model {
                    geometry: WorldGeometry { source: view.source; layer: holder.modelData.layer }
                    opacity: holder.place.opacity === undefined ? 1 : holder.place.opacity
                    materials: DefaultMaterial {
                        lighting: DefaultMaterial.NoLighting
                        cullMode: Material.NoCulling
                        diffuseMap: Texture {
                            source: holder.modelData.picture
                            generateMipmaps: true
                            mipFilter: Texture.Linear
                            // A shape can reach past its cut-out's rectangle;
                            // out there it's see-through, not the far side.
                            tilingModeHorizontal: Texture.ClampToEdge
                            tilingModeVertical: Texture.ClampToEdge
                        }
                    }
                }
            }
        }
    }
}
