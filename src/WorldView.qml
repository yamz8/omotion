import QtQuick
import QtQuick3D
import QtQuick3D.Helpers
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
        environment: ExtendedSceneEnvironment {
            backgroundMode: SceneEnvironment.Transparent
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
            // The scene's look's depth of field: sharp about its focus,
            // blurring away from it by its aperture.
            depthOfFieldEnabled: (view.frame.aperture || 0) > 0 && !view.frame.viewPosition
            depthOfFieldFocusDistance: view.frame.focus || 10
            depthOfFieldFocusRange: Math.max(.3, (view.frame.focus || 10) / Math.max(1, view.frame.aperture || 1))
            depthOfFieldBlurAmount: Math.min(12, (view.frame.aperture || 0) * 2)
        }

        PerspectiveCamera {
            id: camera
            // The window's free view when it's on, else the shot's camera.
            position: view.frame.viewPosition || view.frame.cameraPosition || Qt.vector3d(0, 0, 0)
            rotation: view.frame.viewRotation || view.frame.cameraRotation || Qt.quaternion(1, 0, 0, 0)
            fieldOfView: view.frame.viewFov || view.frame.fov || 40
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

                // A shaped piece is solid: the picture where it was seen,
                // its own colour elsewhere.
                Model {
                    visible: !!holder.modelData.shaped
                    geometry: holder.modelData.shaped ? shapeGeometry : null
                    WorldGeometry { id: shapeGeometry; source: view.source; layer: holder.modelData.shaped ? holder.modelData.layer : -1 }
                    materials: CustomMaterial {
                        shadingMode: CustomMaterial.Unshaded
                        cullMode: Material.NoCulling
                        vertexShader: "qrc:/shape.vert"
                        fragmentShader: "qrc:/shape.frag"
                        property color wrap: holder.place.wrap || "black"
                        property real haze: holder.place.haze || 0
                        property color hazeColour: view.frame.haze || "white"
                        property TextureInput picture: TextureInput {
                            texture: Texture {
                                source: holder.modelData.picture
                                generateMipmaps: true
                                mipFilter: Texture.Linear
                                tilingModeHorizontal: Texture.ClampToEdge
                                tilingModeVertical: Texture.ClampToEdge
                            }
                        }
                    }
                }

                // A flat piece, or the picture itself (solid, so it hides
                // what's behind it and the layer behind its edges).
                Model {
                    visible: !holder.modelData.shaped
                    geometry: WorldGeometry { source: view.source; layer: holder.modelData.shaped ? -1 : holder.modelData.layer }
                    materials: CustomMaterial {
                        shadingMode: CustomMaterial.Unshaded
                        cullMode: Material.NoCulling
                        vertexShader: "qrc:/card.vert"
                        fragmentShader: "qrc:/card.frag"
                        sourceBlend: holder.modelData.opaque ? CustomMaterial.NoBlend : CustomMaterial.One
                        destinationBlend: holder.modelData.opaque ? CustomMaterial.NoBlend : CustomMaterial.OneMinusSrcAlpha
                        property real fade: holder.place.opacity === undefined ? 1 : holder.place.opacity
                        property real haze: holder.place.haze || 0
                        property color hazeColour: view.frame.haze || "white"
                        property TextureInput picture: TextureInput {
                            texture: Texture {
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
}
