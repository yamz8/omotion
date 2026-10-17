import QtQuick
import QtQuick3D
import QtQuick3D.AssetUtils
import Omotion

// A scene's world in 3D: each world layer's mesh with its picture on it, put
// where the frame says, seen through the camera. Exports draw this same view
// without a window, so what's shown is what's written.
Item {
    id: view
    property var source: null
    readonly property var frame: source ? source.frame : ({})
    // A light's position or colour (times its power), black when there's
    // no such light; and how far it reaches.
    function light(i, key) {
        const list = frame.lights || [];
        return i < list.length ? list[i][key] : Qt.vector3d(0, 0, 0);
    }
    function reach(i) {
        const list = frame.lights || [];
        return i < list.length ? list[i].reach : 1;
    }

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
            // The window's free view when it's on, else the shot's camera.
            position: view.frame.viewPosition || view.frame.cameraPosition || Qt.vector3d(0, 0, 0)
            rotation: view.frame.viewRotation || view.frame.cameraRotation || Qt.quaternion(1, 0, 0, 0)
            fieldOfView: view.frame.viewFov || view.frame.fov || 40
            fieldOfViewOrientation: PerspectiveCamera.Vertical
            clipNear: 0.05
            clipFar: 5000
        }

        // Imported models wear their own materials, which want light: a
        // soft sun from the picture's sky, and the scene's lights.
        DirectionalLight {
            visible: (view.source ? view.source.models.length : 0) > 0
            eulerRotation: Qt.vector3d(-55, -30, 0)
            brightness: 0.9
            ambientColor: Qt.rgba(0.45, 0.42, 0.45, 1)
        }
        Repeater3D {
            model: view.source && view.source.models.length > 0 ? (view.frame.lights || []) : []
            delegate: PointLight {
                required property var modelData
                position: modelData.position
                // Its colour times its power, as a brightness and a hue.
                readonly property real power: Math.max(modelData.colour.x, modelData.colour.y, modelData.colour.z)
                color: power > 0 ? Qt.rgba(modelData.colour.x / power, modelData.colour.y / power, modelData.colour.z / power, 1) : "black"
                brightness: power
                quadraticFade: 1 / Math.max(0.01, modelData.reach * modelData.reach)
                linearFade: 0
                constantFade: 1
            }
        }
        Repeater3D {
            model: view.source ? view.source.models : []
            delegate: Node {
                id: placed
                required property var modelData
                readonly property var place: (view.frame.places || {})[String(modelData.layer)] || ({})
                readonly property real size: place.scale === undefined ? 1 : place.scale
                position: place.position || Qt.vector3d(0, 0, 0)
                rotation: place.rotation || Qt.quaternion(1, 0, 0, 0)
                scale: Qt.vector3d(size, size, size)
                visible: place.visible !== false && (place.opacity === undefined || place.opacity > 0)
                opacity: place.opacity === undefined ? 1 : place.opacity
                RuntimeLoader {
                    source: placed.modelData.source
                    position: placed.place.offset || Qt.vector3d(0, 0, 0)
                }
            }
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
                        // Its material, and the lights (four at most).
                        property real shine: holder.place.shine || 0
                        property real roughness: holder.place.roughness === undefined ? 0.5 : holder.place.roughness
                        property real lightCount: (view.frame.lights || []).length
                        // Depth of field (not in the free view): how far is sharp, how much
                        // the rest blurs.
                        property real focus: view.frame.focus || 10
                        property real aperture: view.frame.viewPosition ? 0 : (view.frame.aperture || 0)
                        property vector3d lightAt0: view.light(0, "position")
                        property vector3d lightColour0: view.light(0, "colour")
                        property real lightReach0: view.reach(0)
                        property vector3d lightAt1: view.light(1, "position")
                        property vector3d lightColour1: view.light(1, "colour")
                        property real lightReach1: view.reach(1)
                        property vector3d lightAt2: view.light(2, "position")
                        property vector3d lightColour2: view.light(2, "colour")
                        property real lightReach2: view.reach(2)
                        property vector3d lightAt3: view.light(3, "position")
                        property vector3d lightColour3: view.light(3, "colour")
                        property real lightReach3: view.reach(3)
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
                        property real warpAmount: holder.place.warpAmount || 0
                        property vector2d warpScale: holder.place.warpScale || Qt.vector2d(1, 1)
                        property vector2d warpDrift: holder.place.warpDrift || Qt.vector2d(0, 0)
                        // Its material, and the lights (four at most).
                        property real shine: holder.place.shine || 0
                        property real roughness: holder.place.roughness === undefined ? 0.5 : holder.place.roughness
                        property real lightCount: (view.frame.lights || []).length
                        // Depth of field (not in the free view): how far is sharp, how much
                        // the rest blurs.
                        property real focus: view.frame.focus || 10
                        property real aperture: view.frame.viewPosition ? 0 : (view.frame.aperture || 0)
                        property vector3d lightAt0: view.light(0, "position")
                        property vector3d lightColour0: view.light(0, "colour")
                        property real lightReach0: view.reach(0)
                        property vector3d lightAt1: view.light(1, "position")
                        property vector3d lightColour1: view.light(1, "colour")
                        property real lightReach1: view.reach(1)
                        property vector3d lightAt2: view.light(2, "position")
                        property vector3d lightColour2: view.light(2, "colour")
                        property real lightReach2: view.reach(2)
                        property vector3d lightAt3: view.light(3, "position")
                        property vector3d lightColour3: view.light(3, "colour")
                        property real lightReach3: view.reach(3)

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
